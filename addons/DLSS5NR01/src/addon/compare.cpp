#include "addon/compare.h"
#include "engine/hdr_hlsl.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace nr::compare {

namespace {

// The block every addon of the process maps (the same name in each DLL: this file is compiled into each).
struct Shared {
    volatile LONG magic;
    volatile LONG keyDown;        // a press is being held: the one that set this took it
    volatile LONG active;         // the comparison is on
    volatile LONG current;        // Mode
    volatile LONG present[kModes];
    volatile LONG periodMs, burstMs, settleMs;
    volatile LONG64 sinceMs;      // GetTickCount64 when the current mode began
    uint32_t rgb[kModes];
    char name[kModes][24];
};
constexpr LONG kMagic = 0x43504D31;

Shared* g_shared = nullptr;
HANDLE g_mapping = nullptr;

Shared* S() {
    if (g_shared) return g_shared;
    wchar_t name[64]; swprintf(name, 64, L"Local\\EAM_Compare_%lu", GetCurrentProcessId());
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name);
    if (!g_mapping) return nullptr;
    g_shared = static_cast<Shared*>(MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (g_shared && InterlockedCompareExchange(&g_shared->magic, kMagic, 0) == 0) {   // the first to map it (zeroed by Windows) sets the defaults
        g_shared->present[kNis] = 1;
        g_shared->periodMs = 5000; g_shared->burstMs = 100; g_shared->settleMs = 1500;
        snprintf(g_shared->name[kNis], sizeof g_shared->name[kNis], "NIS"); g_shared->rgb[kNis] = 0xD03030;
    }
    return g_shared;
}

int Lowest() {   // the lowest registered upscaler mode
    Shared* s = S();
    if (!s) return -1;
    for (int m = kDlss; m < kModes; ++m) if (s->present[m]) return m;
    return -1;
}

// The time of a change, in logs\compare_timeline.csv (beside the other logs): when, and which mode, so that the recordings (named by mode) and the logs line up.
void Timeline(const char* what) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (wchar_t* slash = wcsrchr(exe, L'\\')) *slash = 0;
    const std::wstring path = std::wstring(exe) + L"\\logs\\compare_timeline.csv";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"ab") || !f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u.%03u,%lld,%s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, static_cast<long long>(q.QuadPart), what);
    fclose(f);
}

void Enter(Shared* s, int mode, bool on) {
    InterlockedExchange(&s->current, mode);
    InterlockedExchange(&s->active, on ? 1 : 0);
    InterlockedExchange64(&s->sinceMs, static_cast<LONG64>(GetTickCount64()));
    Timeline(on ? s->name[mode] : "OFF");
}

void Advance(Shared* s) {
    if (!s->active) { Enter(s, kNis, true); return; }
    int next = static_cast<int>(s->current) + 1;
    while (next < kModes && !s->present[next]) ++next;
    if (next >= kModes) Enter(s, kNis, false); else Enter(s, next, true);
}

} // namespace

void Register(int mode, const char* name, uint32_t rgb) {
    Shared* s = S();
    if (!s || mode <= kNis || mode >= kModes) return;
    snprintf(s->name[mode], sizeof s->name[mode], "%s", name); s->rgb[mode] = rgb;
    InterlockedExchange(&s->present[mode], 1);
}

void Unregister(int mode) {
    Shared* s = S();
    if (!s || mode <= kNis || mode >= kModes) return;
    InterlockedExchange(&s->present[mode], 0);
    if (s->active && s->current == mode) Enter(s, kNis, true);   // the addon went: NIS until the next press
}

void Poll() {
    Shared* s = S();
    if (!s) return;
    const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_F9) & 0x8000);
    if (down) { if (InterlockedCompareExchange(&s->keyDown, 1, 0) == 0) Advance(s); }
    else if (s->keyDown) InterlockedExchange(&s->keyDown, 0);
}

bool Active() { Shared* s = S(); return s && s->active; }
int Current() { Shared* s = S(); return s ? static_cast<int>(s->current) : kNis; }
const char* Name(int mode) { Shared* s = S(); return s && mode >= 0 && mode < kModes ? s->name[mode] : ""; }

bool MyTurn(int lo, int hi) {
    Shared* s = S();
    if (!s) return true;
    if (s->active) return s->current >= lo && s->current <= hi;
    const int low = Lowest();
    return low < 0 || (low >= lo && low <= hi);
}

bool WantsLabel(int lo, int hi) {
    Shared* s = S();
    if (!s || !s->active) return false;
    if (s->current >= lo && s->current <= hi) return true;
    if (s->current != kNis) return false;
    const int low = Lowest();
    return low >= lo && low <= hi;
}

bool Records(int mode) {
    Shared* s = S();
    if (!s || !s->active) return false;
    for (int m = kDlss; m <= kXess; ++m) if (s->present[m]) return m == mode;
    return false;
}

void StartIn(int mode) {
    Shared* s = S();
    if (s && mode >= kNis && mode < kModes && !s->active) Enter(s, mode, true);   // (the first addon to start does it; the others find it on)
}

void SetCapture(uint32_t periodMs, uint32_t burstMs, uint32_t settleMs) {
    Shared* s = S();
    if (!s) return;
    InterlockedExchange(&s->periodMs, static_cast<LONG>(std::max(0u, periodMs)));
    InterlockedExchange(&s->burstMs, static_cast<LONG>(std::max(1u, burstMs)));
    InterlockedExchange(&s->settleMs, static_cast<LONG>(settleMs));
}

uint64_t ModeAgeMs() {
    Shared* s = S();
    return s ? GetTickCount64() - static_cast<uint64_t>(s->sinceMs) : 0;
}

bool BurstOpen() {
    Shared* s = S();
    if (!s || !s->active) return false;
    const uint64_t age = GetTickCount64() - static_cast<uint64_t>(s->sinceMs);
    if (age < static_cast<uint64_t>(s->settleMs)) return false;
    if (s->periodMs <= 0) return true;   // every frame
    return (age - static_cast<uint64_t>(s->settleMs)) % static_cast<uint64_t>(s->periodMs) < static_cast<uint64_t>(s->burstMs);
}

// ---- the label: a coloured square and the name of what is on, in a 5x7 letter font, drawn into the output's top left (after the corner square the other addons draw)

namespace {

const uint8_t kFont5x7[][5] = {   // columns, the top row is the lowest bit; ' ', then A..Z, 0..9, '+', '-'
    { 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x7E, 0x11, 0x11, 0x11, 0x7E }, { 0x7F, 0x49, 0x49, 0x49, 0x36 }, { 0x3E, 0x41, 0x41, 0x41, 0x22 }, { 0x7F, 0x41, 0x41, 0x22, 0x1C }, { 0x7F, 0x49, 0x49, 0x49, 0x41 },
    { 0x7F, 0x09, 0x09, 0x09, 0x01 }, { 0x3E, 0x41, 0x49, 0x49, 0x7A }, { 0x7F, 0x08, 0x08, 0x08, 0x7F }, { 0x00, 0x41, 0x7F, 0x41, 0x00 }, { 0x20, 0x40, 0x41, 0x3F, 0x01 },
    { 0x7F, 0x08, 0x14, 0x22, 0x41 }, { 0x7F, 0x40, 0x40, 0x40, 0x40 }, { 0x7F, 0x02, 0x0C, 0x02, 0x7F }, { 0x7F, 0x04, 0x08, 0x10, 0x7F }, { 0x3E, 0x41, 0x41, 0x41, 0x3E },
    { 0x7F, 0x09, 0x09, 0x09, 0x06 }, { 0x3E, 0x41, 0x51, 0x21, 0x5E }, { 0x7F, 0x09, 0x19, 0x29, 0x46 }, { 0x46, 0x49, 0x49, 0x49, 0x31 }, { 0x01, 0x01, 0x7F, 0x01, 0x01 },
    { 0x3F, 0x40, 0x40, 0x40, 0x3F }, { 0x1F, 0x20, 0x40, 0x20, 0x1F }, { 0x3F, 0x40, 0x38, 0x40, 0x3F }, { 0x63, 0x14, 0x08, 0x14, 0x63 }, { 0x07, 0x08, 0x70, 0x08, 0x07 },
    { 0x61, 0x51, 0x49, 0x45, 0x43 },
    { 0x3E, 0x51, 0x49, 0x45, 0x3E }, { 0x00, 0x42, 0x7F, 0x40, 0x00 }, { 0x42, 0x61, 0x51, 0x49, 0x46 }, { 0x21, 0x41, 0x45, 0x4B, 0x31 }, { 0x18, 0x14, 0x12, 0x7F, 0x10 },
    { 0x27, 0x45, 0x45, 0x45, 0x39 }, { 0x3C, 0x4A, 0x49, 0x49, 0x30 }, { 0x01, 0x71, 0x09, 0x05, 0x03 }, { 0x36, 0x49, 0x49, 0x49, 0x36 }, { 0x06, 0x49, 0x49, 0x29, 0x1E },
    { 0x08, 0x08, 0x3E, 0x08, 0x08 }, { 0x08, 0x08, 0x08, 0x08, 0x08 },
    { 0x00, 0x60, 0x60, 0x00, 0x00 }, { 0x00, 0x36, 0x36, 0x00, 0x00 }, { 0x20, 0x10, 0x08, 0x04, 0x02 },   // . : /
};
int GlyphOf(char c) { if (c >= 'A' && c <= 'Z') return 1 + (c - 'A'); if (c >= '0' && c <= '9') return 27 + (c - '0'); if (c == '+') return 37; if (c == '-') return 38; if (c == '.') return 39; if (c == ':') return 40; if (c == '/') return 41; return 0; }

const char* const kLabelHlsl = R"HLSL(
Texture2D<float4>   tLabel : register(t0);
RWTexture2D<float4> uOut   : register(u0);
cbuffer C : register(b0) { uint2 origin; uint2 size; uint encoding; float white; float2 unused; };
[numthreads(8, 8, 1)]
void CSLabel(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) return;
    const float4 p = tLabel.Load(int3(id.xy, 0));
    uOut[id.xy + origin] = float4(FromSdr(p.rgb, encoding, white), 1.0);
}
)HLSL";

template <class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// What the pass had bound is taken off while the label is drawn and put back after.
struct SavedBindings {
    static const UINT kSrvs = 8, kUavs = 4;
    ID3D11DeviceContext* ctx;
    ID3D11ShaderResourceView* srvs[kSrvs] = {}; ID3D11UnorderedAccessView* uavs[kUavs] = {};
    ID3D11ComputeShader* shader = nullptr; ID3D11Buffer* cb = nullptr;
    explicit SavedBindings(ID3D11DeviceContext* c) : ctx(c) {
        ctx->CSGetShaderResources(0, kSrvs, srvs); ctx->CSGetUnorderedAccessViews(0, kUavs, uavs); ctx->CSGetShader(&shader, nullptr, nullptr); ctx->CSGetConstantBuffers(0, 1, &cb);
    }
    ~SavedBindings() {
        ctx->CSSetShader(shader, nullptr, 0); ctx->CSSetConstantBuffers(0, 1, &cb);
        ctx->CSSetShaderResources(0, kSrvs, srvs); ctx->CSSetUnorderedAccessViews(0, kUavs, uavs, nullptr);
        for (auto*& v : srvs) Release(v);
        for (auto*& v : uavs) Release(v);
        Release(shader); Release(cb);
    }
};

struct LabelGfx {
    ID3D11Device* dev = nullptr; ID3D11ComputeShader* cs = nullptr; ID3D11Buffer* cb = nullptr; ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr;
    uint32_t w = 0, h = 0; int key = -1;
    void ReleaseAll() { Release(srv); Release(tex); Release(cb); Release(cs); dev = nullptr; w = h = 0; key = -1; }
} g_label;

// The picture of the label: a dark box with rows of text in white, `scale` pixels to a font pixel; the first row has the colour square before it.
std::vector<uint32_t> LabelImage(const std::vector<std::string>& rows, uint32_t rgb, int scale, uint32_t& w, uint32_t& h) {
    const int pad = 2 * scale, sq = 7 * scale, gap = 2 * scale, line = 9 * scale;
    int widest = 0;
    for (size_t r = 0; r < rows.size(); ++r) widest = std::max(widest, static_cast<int>(rows[r].size()) * 6 * scale + (r == 0 ? sq + gap : 0));
    w = static_cast<uint32_t>(pad + widest + pad); h = static_cast<uint32_t>(pad + static_cast<int>(rows.size()) * line + pad - 2 * scale);
    const uint32_t bg = 0xFF181818u, fg = 0xFFFFFFFFu, square = 0xFF000000u | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);   // (in memory R, G, B, A)
    std::vector<uint32_t> px(static_cast<size_t>(w) * h, bg);
    for (int y = 0; y < sq; ++y) for (int x = 0; x < sq; ++x) px[static_cast<size_t>(pad + y) * w + pad + x] = square;
    for (size_t r = 0; r < rows.size(); ++r) {
        const int top = pad + static_cast<int>(r) * line, left = pad + (r == 0 ? sq + gap : 0);
        for (size_t i = 0; i < rows[r].size(); ++i) {
            const uint8_t* col = kFont5x7[GlyphOf(rows[r][i])];
            for (int cx = 0; cx < 5; ++cx) for (int cy = 0; cy < 7; ++cy) if (col[cx] & (1 << cy))
                for (int dy = 0; dy < scale; ++dy) for (int dx = 0; dx < scale; ++dx)
                    px[static_cast<size_t>(top + cy * scale + dy) * w + left + (static_cast<int>(i) * 6 + cx) * scale + dx] = fg;
        }
    }
    return px;
}

bool CompileLabelShader(ID3D11Device* dev) {
    ID3DBlob* code = nullptr, * error = nullptr;
    const std::string source = std::string(NR_HDR_HLSL) + kLabelHlsl;
    if (FAILED(D3DCompile(source.c_str(), source.size(), "compare_label", nullptr, nullptr, "CSLabel", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error))) { Release(error); return false; }
    const HRESULT hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_label.cs);
    code->Release();
    return SUCCEEDED(hr);
}

} // namespace

void DrawLabel(ID3D11DeviceContext* ctx, uint32_t outX, uint32_t outY, uint32_t outW, uint32_t outH, uint32_t encoding, float whiteNits) {
    Shared* s = S();
    if (!s || !s->active || !ctx) return;
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev); if (dev) dev->Release();
    if (!dev) return;
    SavedBindings saved(ctx);
    ID3D11UnorderedAccessView* outUav = saved.uavs[0];
    if (!outUav) return;
    if (g_label.dev != dev) {
        g_label.ReleaseAll(); g_label.dev = dev;
        D3D11_BUFFER_DESC cb{}; cb.ByteWidth = 32; cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&cb, nullptr, &g_label.cb)) || !CompileLabelShader(dev)) { g_label.ReleaseAll(); return; }
    }
    const int mode = static_cast<int>(s->current);
    const int scale = std::clamp(static_cast<int>(outW / 800), 2, 8);
    const int key = mode * 16 + scale;
    if (key != g_label.key || !g_label.tex) {
        std::vector<std::string> rows = { s->name[mode] };
        uint32_t w = 0, h = 0; const std::vector<uint32_t> px = LabelImage(rows, s->rgb[mode], scale, w, h);
        Release(g_label.srv); Release(g_label.tex);
        D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA init{ px.data(), w * 4, 0 };
        if (FAILED(dev->CreateTexture2D(&d, &init, &g_label.tex)) || FAILED(dev->CreateShaderResourceView(g_label.tex, nullptr, &g_label.srv))) { Release(g_label.srv); Release(g_label.tex); return; }
        g_label.w = w; g_label.h = h; g_label.key = key;
    }
    const uint32_t square = std::max(12u, outW / 150u);   // the corner square the other addons draw: the label starts after it
    const uint32_t ox = outX + square + 12, oy = outY + 4;
    if (ox + g_label.w > outX + outW || oy + g_label.h > outY + outH) return;
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(g_label.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        uint32_t* c = static_cast<uint32_t*>(m.pData);
        c[0] = ox; c[1] = oy; c[2] = g_label.w; c[3] = g_label.h; c[4] = encoding; memcpy(&c[5], &whiteNits, 4); c[6] = c[7] = 0;
        ctx->Unmap(g_label.cb, 0);
    }
    ID3D11ShaderResourceView* none[SavedBindings::kSrvs] = {}; ID3D11UnorderedAccessView* noUav[SavedBindings::kUavs] = {};
    ctx->CSSetShaderResources(0, SavedBindings::kSrvs, none); ctx->CSSetUnorderedAccessViews(0, SavedBindings::kUavs, noUav, nullptr);
    ctx->CSSetShader(g_label.cs, nullptr, 0); ctx->CSSetConstantBuffers(0, 1, &g_label.cb);
    ctx->CSSetShaderResources(0, 1, &g_label.srv); ctx->CSSetUnorderedAccessViews(0, 1, &outUav, nullptr);
    ctx->Dispatch((g_label.w + 7) / 8, (g_label.h + 7) / 8, 1);
}

} // namespace nr::compare
