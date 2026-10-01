// Offline host for DLSS5NR01.dll: fake IHost + headless ImGui frames + a synthetic "LSFG" dispatch
// pattern and a real swap chain on the display GPU. Exercises init, panel rendering, the dispatch callback (called here as the manager does),
// FrameTap auto-assignment, the read-only tap, the D3D11<->D3D12 bridge, NR itself, the Present hook and the
// present-time compose (generated frames first, the real frame last, like LSFG X3).
#include <windows.h>
#include <intrin.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <thread>
#include <atomic>
#include "imgui.h"
#include "eam/addon_sdk.h"
#include "eam/widgets.h"
#include "ui_shot.h"
#include "addon/lsrec.h"
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")

struct FakeHost : IHost {
    struct Sub { uint32_t id; EamEventCallback cb; void* ud; };
    std::vector<Sub> subs; std::map<std::string, std::string> cfg; void* dev = nullptr; void* ctx = nullptr;
    std::string lastPattern;
    void Log(EamLogLevel, const char* m) override { printf("[addon] %s\n", m); fflush(stdout); logged.push_back(m); }
    std::vector<std::string> logged;
    bool Said(const char* text) const { for (const auto& l : logged) if (l.find(text) != std::string::npos) return true; return false; }
    const char* GetConfig(const char*, const char* k, const char* d) override { auto it = cfg.find(k); return it == cfg.end() ? d : it->second.c_str(); }
    void SetConfig(const char*, const char* k, const char* v) override { cfg[k] = v; }
    void SaveConfig() override {}
    uint32_t GetHostVersion() override { return 0x10200; }   // API 1.2: images for the panel
    void SubscribeEvent(uint32_t id, EamEventCallback cb, void* ud) override { subs.push_back({ id, cb, ud }); }
    void UnsubscribeEvent(uint32_t id, EamEventCallback cb) override { for (size_t i = 0; i < subs.size();) if (subs[i].id == id && subs[i].cb == cb) subs.erase(subs.begin() + i); else ++i; }
    void PublishEvent(uint32_t id, const void* d, uint32_t n) override { auto copy = subs; for (auto& s : copy) if (s.id == id) s.cb(id, d, n, s.ud); }
    void* GetD3D11Device() override { return dev; }
    void* GetD3D11DeviceContext() override { return ctx; }
    // Dispatch callbacks, as the manager runs them: around each of "Lossless Scaling's" passes (the test's), with the context known inside.
    struct Pre { HMODULE module; EamPreDispatchCallback cb; void* ud; };
    std::vector<Pre> pres; ID3D11DeviceContext* dispatching = nullptr; uint32_t dispatches = 0;
    void SetPreDispatchCallback(EamPreDispatchCallback cb, void* ud) override {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)_ReturnAddress(), &module);
        for (size_t i = 0; i < pres.size();) if (pres[i].module == module) pres.erase(pres.begin() + i); else ++i;
        if (cb) pres.push_back({ module, cb, ud });
    }
    void SetPostDispatchCallback(EamPostDispatchCallback, void*) override {}
    void* GetCurrentComputeShader() override { return nullptr; }
    uint32_t GetDispatchCount() override { return dispatches; }
    void* GetDispatchingContext() override { return dispatching; }
    // images for the panel, on the shot renderer's device (none without shot=)
    ID3D11Device* imageDevice = nullptr;
    void* CreateImage(const void* rgba, uint32_t w, uint32_t h, uint32_t pitch) override {
        if (!imageDevice || !rgba) return nullptr;
        D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA px{ rgba, pitch, 0 };
        ID3D11Texture2D* t = nullptr; ID3D11ShaderResourceView* v = nullptr;
        if (SUCCEEDED(imageDevice->CreateTexture2D(&d, &px, &t))) { imageDevice->CreateShaderResourceView(t, nullptr, &v); t->Release(); }
        if (v) ++images;
        return v;
    }
    void ReleaseImage(void* image) override { if (image) static_cast<IUnknown*>(image)->Release(); }
    int images = 0;
    double longestCallMs = 0;   // the longest the addon held up a dispatch (Lossless Scaling's render thread)
    void Dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
        ++dispatches; dispatching = c;
        const auto t0 = std::chrono::steady_clock::now();
        bool skip = false;
        for (const Pre& p : pres) skip = p.cb(x, y, z, p.ud) || skip;
        longestCallMs = std::max(longestCallMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        dispatching = nullptr;
        if (!skip) c->Dispatch(x, y, z);
    }
    // live status and metrics, recorded so the run can check that the addon reports them
    std::map<std::string, int> metricCount; std::map<std::string, double> metricLast; std::string status; int statusLevel = -1; int statusCalls = 0;
    void SetStatus(const char*, const char* text, int level) override { status = text; statusLevel = level; ++statusCalls; }
    void PublishMetric(const char*, const char* key, double value, const char*) override { metricCount[key]++; metricLast[key] = value; }
};

typedef void (*PFN_Init)(IHost*, ImGuiContext*, void*, void*, void*);
typedef void (*PFN_Void)();
typedef uint32_t (*PFN_Caps)();

static ID3D11ComputeShader* MakeCS(ID3D11Device* dev, const char* src) {
    ID3DBlob* code = nullptr, * err = nullptr;
    if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err))) { printf("compile failed: %s\n", err ? (char*)err->GetBufferPointer() : "?"); return nullptr; }
    ID3D11ComputeShader* cs = nullptr; dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs); code->Release(); return cs;
}
static ID3D11Texture2D* MakeTex(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT f, bool uav) {
    D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
    ID3D11Texture2D* t = nullptr; dev->CreateTexture2D(&d, nullptr, &t); return t;
}
static uint32_t Pattern(UINT x, UINT y, UINT w, UINT h) { uint8_t r = (uint8_t)(x * 255 / w), g = (uint8_t)(y * 255 / h), b = ((x / 32 + y / 32) & 1) ? 200 : 60; return 0xFF000000u | (r << 16) | (g << 8) | b; }
// nismove=1: a picture that slides kMoveX, kMoveY pixels a frame (right and down), by fractions of a pixel as a camera does: blocks of 3 pixels in
// hashed colours, point-sampled at every pixel's centre with no smoothing (aliased, as an old game draws), so each frame catches the edges at a
// different phase. What DLSS should rebuild is the same picture sampled at the output's pixel centres (Ideal). Frame t's picture at (x, y) is
// what frame t-1 had at (x - kMoveX, y - kMoveY).
static const double kMoveX = 5.37, kMoveY = 2.21, kCell = 3.0;
static uint32_t Hash(uint32_t x, uint32_t y) { uint32_t h = x * 374761393u + y * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16); }
static uint32_t PictureAt(double u, double v) {   // the continuous picture at (u, v) in input pixels
    const uint32_t big = Hash((uint32_t)(int64_t)std::floor(u / kCell + 100000), (uint32_t)(int64_t)std::floor(v / kCell + 100000));
    const uint32_t r = 30 + (big & 0xBF), g = 30 + ((big >> 8) & 0xBF), b = 30 + ((big >> 16) & 0xBF);
    return 0xFF000000u | (r << 16) | (g << 8) | b;   // in memory B, G, R, A
}
static uint32_t MovingPixel(int x, int y, int t) { return PictureAt(x + 0.5 - t * kMoveX, y + 0.5 - t * kMoveY); }
static uint32_t Ideal(int ox, int oy, double scale, int t) { return PictureAt((ox + 0.5) / scale - t * kMoveX, (oy + 0.5) / scale - t * kMoveY); }
static void FillMoving(ID3D11DeviceContext* ctx, ID3D11Texture2D* t, UINT w, UINT h, int frame) {
    std::vector<uint32_t> px(w * h); for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) px[y * w + x] = MovingPixel((int)x, (int)y, frame);
    ctx->UpdateSubresource(t, 0, nullptr, px.data(), w * 4, 0);
}
static void Fill(ID3D11DeviceContext* ctx, ID3D11Texture2D* t, UINT w, UINT h) {   // gradient + stripes so NR has something to look at
    std::vector<uint32_t> px(w * h); for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) px[y * w + x] = Pattern(x, y, w, h);
    ctx->UpdateSubresource(t, 0, nullptr, px.data(), w * 4, 0);
}
// hdr=: the pattern as an HDR frame, in the encoding's own values per channel (scRGB: 1.0 = 80 nits; PQ: 0..1 of the 10-bit code), with a band
// across the middle of highlights at 1000 nits, far above any SDR white. kHdrWhite is the white the pattern's own white is put at.
static const float kHdrWhite = 80.0f;
static bool InHdrBand(UINT y, UINT h) { return y >= h * 45 / 100 && y < h * 55 / 100; }
static float SrgbToLin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float NitsToPq(float n) {
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    const float y = powf(std::clamp(n / 10000.0f, 0.0f, 1.0f), m1);
    return powf((c1 + c2 * y) / (1.0f + c3 * y), m2);
}
static void HdrPixel(UINT x, UINT y, UINT w, UINT h, bool pq, float out[3]) {
    const uint32_t p = Pattern(x, y, w, h);
    float lin[3] = { SrgbToLin(((p >> 16) & 255) / 255.0f), SrgbToLin(((p >> 8) & 255) / 255.0f), SrgbToLin((p & 255) / 255.0f) };
    const bool band = InHdrBand(y, h);
    if (!pq) { for (int i = 0; i < 3; ++i) out[i] = band ? 12.5f : lin[i] * kHdrWhite / 80.0f; return; }
    const float m[3][3] = { { 0.6274f, 0.3293f, 0.0433f }, { 0.0691f, 0.9195f, 0.0114f }, { 0.0164f, 0.0880f, 0.8956f } };
    for (int i = 0; i < 3; ++i) out[i] = std::round(NitsToPq(band ? 1000.0f : (m[i][0] * lin[0] + m[i][1] * lin[1] + m[i][2] * lin[2]) * kHdrWhite) * 1023.0f) / 1023.0f;
}
static ID3D11Texture2D* MakeHdrPicture(ID3D11Device* dev, ID3D11DeviceContext* dc, UINT w, UINT h, bool pq) {
    D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.Format = pq ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* t = nullptr; if (FAILED(dev->CreateTexture2D(&d, nullptr, &t))) return nullptr;
    std::vector<uint64_t> px(w * h);
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
        float v[3]; HdrPixel(x, y, w, h, pq, v);
        if (pq) { uint32_t r = (uint32_t)std::lround(v[0] * 1023), g = (uint32_t)std::lround(v[1] * 1023), b = (uint32_t)std::lround(v[2] * 1023);
                  reinterpret_cast<uint32_t*>(px.data())[y * w + x] = r | g << 10 | b << 20 | 3u << 30; }
        else { using namespace DirectX::PackedVector;
               px[y * w + x] = (uint64_t)XMConvertFloatToHalf(v[0]) | (uint64_t)XMConvertFloatToHalf(v[1]) << 16 | (uint64_t)XMConvertFloatToHalf(v[2]) << 32 | (uint64_t)XMConvertFloatToHalf(1.0f) << 48; }
    }
    dc->UpdateSubresource(t, 0, nullptr, px.data(), w * (pq ? 4 : 8), 0);
    return t;
}
// Reads an HDR buffer back against the picture: how many pixels outside the band changed, how many inside it, and any NaN or infinity.
struct HdrCheck { uint64_t base = 0, baseChanged = 0, band = 0, bandChanged = 0, bad = 0; double baseChange = 0; };
static HdrCheck CheckHdr(ID3D11Device* dev, ID3D11DeviceContext* dc, ID3D11Texture2D* t, UINT w, UINT h, bool pq) {
    HdrCheck r;
    D3D11_TEXTURE2D_DESC sd; t->GetDesc(&sd); sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr; dev->CreateTexture2D(&sd, nullptr, &st); if (!st) return r;
    dc->CopyResource(st, t); D3D11_MAPPED_SUBRESOURCE m{}; if (FAILED(dc->Map(st, 0, D3D11_MAP_READ, 0, &m))) { st->Release(); return r; }
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
        float want[3], got[3]; HdrPixel(x, y, w, h, pq, want);
        const char* row = (const char*)m.pData + y * m.RowPitch;
        if (pq) { const uint32_t v = reinterpret_cast<const uint32_t*>(row)[x]; for (int i = 0; i < 3; ++i) got[i] = ((v >> (10 * i)) & 1023) / 1023.0f; }
        else { using namespace DirectX::PackedVector; const uint16_t* v = reinterpret_cast<const uint16_t*>(row) + x * 4; for (int i = 0; i < 3; ++i) got[i] = XMConvertHalfToFloat(v[i]); }
        bool changed = false; double diff = 0;
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(got[i])) { r.bad++; changed = true; continue; }
            const float tol = pq ? 1.5f / 1023.0f : 0.002f + 0.004f * fabsf(want[i]);
            if (fabsf(got[i] - want[i]) > tol) changed = true;
            diff += fabsf(got[i] - want[i]);
        }
        if (InHdrBand(y, h)) { r.band++; if (changed) r.bandChanged++; }
        else { r.base++; if (changed) { r.baseChanged++; r.baseChange += diff / 3.0; } }
    }
    dc->Unmap(st, 0); st->Release();
    return r;
}

// Reads a BGRA8 texture back and counts the pixels that differ from the pattern.
static uint64_t CountChanged(ID3D11Device* dev, ID3D11DeviceContext* dc, ID3D11Texture2D* t, UINT w, UINT h, double* meanAbs, const char* bmpName) {
    D3D11_TEXTURE2D_DESC sd{}; sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr; dev->CreateTexture2D(&sd, nullptr, &st); if (!st) return 0;
    dc->CopyResource(st, t); D3D11_MAPPED_SUBRESOURCE m{}; if (FAILED(dc->Map(st, 0, D3D11_MAP_READ, 0, &m))) { st->Release(); return 0; }
    uint64_t changed = 0, sumAbs = 0;
    for (UINT y = 0; y < h; ++y) { const uint32_t* row = (const uint32_t*)((const char*)m.pData + y * m.RowPitch); for (UINT x = 0; x < w; ++x) { uint32_t want = Pattern(x, y, w, h), got = row[x]; if (got != want) { changed++; sumAbs += abs((int)((got >> 16) & 255) - (int)((want >> 16) & 255)) + abs((int)((got >> 8) & 255) - (int)((want >> 8) & 255)) + abs((int)(got & 255) - (int)(want & 255)); } } }
    if (bmpName) {
        FILE* fp = fopen(bmpName, "wb");
        if (fp) { uint32_t rowBytes = w * 4, imgSize = rowBytes * h; uint8_t fh[14] = { 'B','M' }; uint32_t fsz = 54 + imgSize; memcpy(fh + 2, &fsz, 4); uint32_t off = 54; memcpy(fh + 10, &off, 4);
            uint8_t ih[40] = {}; uint32_t v = 40; memcpy(ih, &v, 4); int32_t wv = (int32_t)w, hv = -(int32_t)h; memcpy(ih + 4, &wv, 4); memcpy(ih + 8, &hv, 4); uint16_t planes = 1, bpp = 32; memcpy(ih + 12, &planes, 2); memcpy(ih + 14, &bpp, 2); memcpy(ih + 20, &imgSize, 4);
            fwrite(fh, 1, 14, fp); fwrite(ih, 1, 40, fp); for (UINT y = 0; y < h; ++y) fwrite((const char*)m.pData + y * m.RowPitch, 1, rowBytes, fp); fclose(fp); }
    }
    dc->Unmap(st, 0); st->Release();
    if (meanAbs) *meanAbs = changed ? (double)sumAbs / changed / 3.0 : 0.0;
    return changed;
}

// A crash on any thread says where before the process goes: the module and offset (and what an access violation touched), so a rare fault in a test run can be found in the code.
static LONG CALLBACK ReportFault(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != 0xC0000409 && code != 0xC0000374 && code != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    static LONG once = 0; if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE mod = nullptr; char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(info->ExceptionRecord->ExceptionAddress), &mod) && mod) GetModuleFileNameA(mod, name, sizeof name);
    const char* slash = strrchr(name, 92);
    printf("[hosttest] FAULT 0x%08lx at %s+0x%llx on thread %lu", code, slash ? slash + 1 : name, (unsigned long long)((char*)info->ExceptionRecord->ExceptionAddress - (char*)mod), GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) printf(" (%s address 0x%llx)", info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", (unsigned long long)info->ExceptionRecord->ExceptionInformation[1]);
    printf("\n");
    // the stack from the fault itself (x64 unwind data), not from this handler
    CONTEXT ctx = *info->ContextRecord;
    for (int i = 0; i < 28 && ctx.Rip; ++i) {
        HMODULE m2 = nullptr; char n2[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(ctx.Rip), &m2) && m2) GetModuleFileNameA(m2, n2, sizeof n2);
        const char* s2 = strrchr(n2, 92);
        printf("[hosttest]   #%d %s+0x%llx", i, s2 ? s2 + 1 : n2, (unsigned long long)(ctx.Rip - reinterpret_cast<DWORD64>(m2)));
        printf("%c", 10);
        DWORD64 base = 0; PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        if (!fn) { ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp); ctx.Rsp += 8; continue; }   // a leaf with no unwind data: the return address is on top
        void* handlerData = nullptr; DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handlerData, &frame, nullptr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char** argv) {
    AddVectoredExceptionHandler(1, ReportFault);
    setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: a crash must not swallow the last lines
    const char* dllPath = argc > 1 ? argv[1] : "DLSS5NR01.dll";
    HMODULE h = LoadLibraryA(dllPath); if (!h) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    auto Init = (PFN_Init)GetProcAddress(h, "AddonInitialize"); auto Shut = (PFN_Void)GetProcAddress(h, "AddonShutdown"); auto Render = (PFN_Void)GetProcAddress(h, "AddonRenderSettings"); auto Caps = (PFN_Caps)GetProcAddress(h, "GetAddonCapabilities");
    if (!Init || !Shut || !Render || !Caps) { printf("exports missing\n"); return 1; }
    printf("caps 0x%x\n", Caps());

    // usage: nr_hosttest <addon dll> [-] [snippet path] [key=value ...]     (argv[2] is kept for old scripts and ignored)
    // Two keys are the host's own: shot=<file.bmp> renders the settings panel at the end through a themed offscreen
    // renderer (shotW= shotH= set the size; the panel is laid out at its natural height and every section is opened).
    std::string shotPath; int shotW = 760, shotH = 3400;
    for (int i = 4; i < argc; ++i) { if (!strncmp(argv[i], "shot=", 5)) shotPath = argv[i] + 5; else if (!strncmp(argv[i], "shotW=", 6)) shotW = atoi(argv[i] + 6); else if (!strncmp(argv[i], "shotH=", 6)) shotH = atoi(argv[i] + 6); }
    const bool shotMode = !shotPath.empty();
    bool openSections = true;   // sectionsOpen=0 leaves every collapsible section as it is on first start (closed)
    for (int i = 4; i < argc; ++i) if (!strcmp(argv[i], "sectionsOpen=0")) openSections = false;
    // flowsplit=1: the right half of the fake LSFG flow has both fields pointing the same way (they disagree), the left half agrees
    bool flowSplit = false;
    for (int i = 4; i < argc; ++i) if (!strncmp(argv[i], "flowsplit=", 10)) flowSplit = atoi(argv[i] + 10) != 0;
    // hdr=scrgb|pq: after the rest, the presented frames in HDR (see [check-hdr])
    int hdrMode = 0;
    for (int i = 4; i < argc; ++i) { if (!strcmp(argv[i], "hdr=scrgb")) hdrMode = 1; if (!strcmp(argv[i], "hdr=pq")) hdrMode = 2; }
    // replay=<file.lsrec>: after the rest, a recording's frames presented (see [check-replay]); replayOut=<folder>: pictures of the result
    std::string replayPath, replayOut;
    for (int i = 4; i < argc; ++i) { if (!strncmp(argv[i], "replay=", 7)) replayPath = argv[i] + 7; if (!strncmp(argv[i], "replayOut=", 10)) replayOut = argv[i] + 10; }

    // ImGui: headless normally; with shot= it renders through the DX11 backend into an offscreen target, in the manager's theme
    ImGuiContext* ctx = ImGui::CreateContext(); ImGuiIO& io = ImGui::GetIO(); io.DisplaySize = ImVec2(1280, 800); io.DeltaTime = 1.0f / 60; io.IniFilename = nullptr;
    UiShot shot;
    if (shotMode) {
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf");
        eam::ui::theme::ApplyStyle(ImGui::GetStyle()); ImGui::GetStyle().FontSizeBase = 15.0f;
        if (!shot.Init(shotW, shotH)) { printf("shot renderer init failed\n"); return 1; }
    } else {
        io.Fonts->AddFontDefault();
        unsigned char* px; int tw, th; io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);   // headless: build the atlas ourselves
    }
    ImGuiMemAllocFunc af; ImGuiMemFreeFunc ff; void* ud; ImGui::GetAllocatorFunctions(&af, &ff, &ud);
    FakeHost host; host.cfg["snippetPath"] = argc > 3 ? argv[3] : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Lossless Scaling\\nvngx_dlssnr.dll";
    for (int i = 4; i < argc; ++i) {   // extra key=value pairs override addon config (workingScale=0.5 debugView=3 ...)
        const char* eq = strchr(argv[i], '='); if (!eq) continue;
        if (!strncmp(argv[i], "shot", 4) || !strncmp(argv[i], "nisnoflow", 9) || !strncmp(argv[i], "nisbgra", 7) || !strncmp(argv[i], "nismove", 7) || !strncmp(argv[i], "nisW", 4) || !strncmp(argv[i], "nisH", 4) || !strncmp(argv[i], "nisScale", 8) || !strncmp(argv[i], "nisvp", 5) || !strncmp(argv[i], "nisedge", 7) || !strncmp(argv[i], "nisline", 7) || !strncmp(argv[i], "unload", 6) || !strncmp(argv[i], "nisgap", 6) || !strncmp(argv[i], "nisswitch", 9) || !strncmp(argv[i], "gpuload", 7) || !strncmp(argv[i], "offframes", 9) || !strncmp(argv[i], "devflags", 8) || !strncmp(argv[i], "second", 6) || !strncmp(argv[i], "flowsplit", 9) || !strncmp(argv[i], "exitmode", 8) || !strncmp(argv[i], "sectionsOpen", 12) || !strncmp(argv[i], "hdr=", 4) || !strncmp(argv[i], "replay", 6) || !strncmp(argv[i], "warp", 4) || !strncmp(argv[i], "nishdr", 6)) continue;   // the host's own keys
        host.cfg[std::string(argv[i], (size_t)(eq - argv[i]))] = eq + 1; printf("cfg %.*s = %s\n", (int)(eq - argv[i]), argv[i], eq + 1);
    }
    // a 10-bit frame is HDR10 only when the display runs in HDR, and the test's display may not: the addon is told so, as a user can
    for (int i = 4; i < argc; ++i) if (!strcmp(argv[i], "nishdr=pq") && !host.cfg.count("frameEncoding")) { host.cfg["frameEncoding"] = "2"; printf("cfg frameEncoding = 2 (nishdr=pq)\n"); }
    if (hdrMode == 2 && !host.cfg.count("frameEncoding")) { host.cfg["frameEncoding"] = "2"; printf("cfg frameEncoding = 2 (hdr=pq)\n"); }
    if (shotMode) host.imageDevice = shot.dev;
    Init(&host, ctx, (void*)af, (void*)ff, ud);
    // second=<dll>: the other addon of the pair, loaded as well. Only one may work on the frames: the second, found the first in charge at its
    // start, must switch itself off.
    PFN_Void Shut2 = nullptr; std::string secondDll;
    for (int i = 4; i < argc; ++i) if (!strncmp(argv[i], "second=", 7)) secondDll = argv[i] + 7;
    if (!secondDll.empty()) {
        HMODULE h2 = LoadLibraryA(secondDll.c_str()); if (!h2) { printf("LoadLibrary %s failed %lu\n", secondDll.c_str(), GetLastError()); return 1; }
        auto Init2 = (PFN_Init)GetProcAddress(h2, "AddonInitialize"); Shut2 = (PFN_Void)GetProcAddress(h2, "AddonShutdown");
        if (!Init2 || !Shut2) { printf("exports of %s missing\n", secondDll.c_str()); return 1; }
        Init2(&host, ctx, (void*)af, (void*)ff, ud);
        printf("[check-pair] second addon %s at its start: %s\n", secondDll.c_str(), host.Said("switched off at start:") ? "STEPPED ASIDE" : "BOTH ON");
    }

    auto panel = [&]() {
        if (shotMode) {
            ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2((float)shotW, 0.0f));
            ImGui::Begin("Addon Manager", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
            for (const char* h : { "Model (what it does to the picture)", "Motion", "Quality and performance", "Picture (sharpness, tone, colour, grain)", "Keep the HUD untouched", "Upscaling", "Compare and hotkeys", "Recording (for bug reports and tests)", "Games (a look per program)", "Frame detection (advanced)", "Technical status", "Advanced" })
                if (openSections) ImGui::GetStateStorage()->SetInt(ImGui::GetID(h), 1);
        } else { ImGui::SetNextWindowSize(ImVec2(900, 700)); ImGui::Begin("Addon Manager"); }
        Render(); ImGui::End();
    };
    auto frame = [&](const char* tag) {
        if (shotMode) { shot.Frame(panel, 1); printf("[frame %s] rendered\n", tag); return; }
        ImGui::NewFrame(); panel(); ImGui::Render();
        ImDrawData* dd = ImGui::GetDrawData(); printf("[frame %s] %d draw lists, %d vertices\n", tag, dd->CmdListsCount, dd->TotalVtxCount);
    };
    auto emptyFrame = [&]() { if (shotMode) shot.Frame([] {}, 1); else { ImGui::NewFrame(); ImGui::Render(); } };
    frame("before device");

    // device on adapter 0 (the display 3090) and DEVICE_READY
    IDXGIFactory2* f = nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&f)); IDXGIAdapter1* a = nullptr; f->EnumAdapters1(0, &a);
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* dc = nullptr; D3D_FEATURE_LEVEL fl;
    UINT deviceFlags = 0;   // devflags=<n>: create the device as Lossless Scaling might (1 = D3D11_CREATE_DEVICE_SINGLETHREADED)
    for (int i = 4; i < argc; ++i) if (!strncmp(argv[i], "devflags=", 9)) deviceFlags = (UINT)atoi(argv[i] + 9);
    bool warp = false;      // warp=1: Lossless Scaling's device on Windows' software renderer, a card that is not NVIDIA's
    for (int i = 4; i < argc; ++i) if (!strcmp(argv[i], "warp=1")) warp = true;
    if (FAILED(D3D11CreateDevice(warp ? nullptr : a, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, nullptr, deviceFlags, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &dc))) { printf("device failed\n"); return 1; }
    if (warp) printf("[hosttest] device on WARP (not an NVIDIA card)\n");
    if (deviceFlags) printf("[hosttest] device created with flags 0x%x\n", deviceFlags);
    ID3D11Multithread* mt = nullptr; dc->QueryInterface(IID_PPV_ARGS(&mt)); mt->SetMultithreadProtected(TRUE); mt->Release();   // like LS/WGC
    host.dev = dev; host.ctx = dc; host.PublishEvent(EAM_EVENT_D3D11_DEVICE_READY, nullptr, 0);
    // a few panel frames before the first pass (the engine starts at the first dispatch, so waiting longer here only slows the run)
    for (int i = 0; i < 4; ++i) { frame("engine loading"); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }

    // a real (small, visible) window + flip swap chain: the addon hooks Present and composes into its back buffer
    const UINT W = 1920, H = 1080, FW = 480, FH = 270;
    WNDCLASSEXW wc{ sizeof wc }; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"NrHostTest"; RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"nr hosttest", WS_POPUP, 0, 0, 320, 180, nullptr, nullptr, wc.hInstance, nullptr);
    // never shown: the swap chain presents fine without being visible, and a test must not put windows on anyone's screen
    bool barsMode = false; for (int i = 4; i < argc; ++i) if (!strcmp(argv[i], "bars=1")) barsMode = true;   // bars=1: the screen is wider than the frame (2560x1080 for 1920x1080), the frame drawn in the middle with black bars, as Lossless Scaling does
    const UINT SW = barsMode ? 2560 : W;
    DXGI_SWAP_CHAIN_DESC1 scd{}; scd.Width = SW; scd.Height = H; scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; scd.SampleDesc.Count = 1; scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; scd.BufferCount = 2; scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; scd.Scaling = DXGI_SCALING_STRETCH;
    IDXGISwapChain1* sc = nullptr;
    if (FAILED(f->CreateSwapChainForHwnd(dev, hwnd, &scd, nullptr, nullptr, &sc))) { printf("swap chain failed\n"); return 1; }
    auto pump = [&]() { MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } };

    // synthetic LSFG: pyramid pass (TAP), flow passes, then per output frame a compose; 3 presents per real frame
    ID3D11Texture2D* prev = MakeTex(dev, W, H, DXGI_FORMAT_B8G8R8A8_UNORM, true), * cur = MakeTex(dev, W, H, DXGI_FORMAT_B8G8R8A8_UNORM, true);
    ID3D11Texture2D* flow = MakeTex(dev, FW, FH, DXGI_FORMAT_R16G16_FLOAT, true), * out = MakeTex(dev, W, H, DXGI_FORMAT_B8G8R8A8_UNORM, true);
    Fill(dc, prev, W, H); Fill(dc, cur, W, H);
    auto srv = [&](ID3D11Texture2D* t) { ID3D11ShaderResourceView* v = nullptr; dev->CreateShaderResourceView(t, nullptr, &v); return v; };
    auto uav = [&](ID3D11Texture2D* t) { ID3D11UnorderedAccessView* v = nullptr; dev->CreateUnorderedAccessView(t, nullptr, &v); return v; };
    ID3D11ShaderResourceView* sPrev = srv(prev), * sCur = srv(cur); ID3D11UnorderedAccessView* uFlow = uav(flow), * uOut = uav(out);
    ID3D11ComputeShader* csFlow = MakeCS(dev, "Texture2D<float4> a:register(t0); Texture2D<float4> b:register(t1); RWTexture2D<float2> o:register(u0); [numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){ o[id.xy]=float2(a[id.xy*4].r-b[id.xy*4].r,0); }");
    if (!csFlow) return 1;
    ID3D11ShaderResourceView* nulls[3] = {}; ID3D11UnorderedAccessView* nullu[1] = {};
    // LSFG3 as seen in LS 3.x logs: per real frame one pyramid pass (full frame SRV -> 4 smaller R8 UAVs),
    // then per generated frame a full-size compose (two full frames + the flow -> full UAV), plus low-res work.
    ID3D11Texture2D* pyr[4]; ID3D11UnorderedAccessView* uPyr[4];
    for (int i = 0; i < 4; ++i) { UINT pw = (W * 7 / 10) >> i, ph = (H * 7 / 10) >> i; pyr[i] = MakeTex(dev, pw, ph, DXGI_FORMAT_R8_UNORM, true); uPyr[i] = uav(pyr[i]); }
    ID3D11ComputeShader* csPyr = MakeCS(dev, "Texture2D<float4> a:register(t0); RWTexture2D<float> o0:register(u0); RWTexture2D<float> o1:register(u1); RWTexture2D<float> o2:register(u2); RWTexture2D<float> o3:register(u3); [numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){ float l=a[id.xy*2].g; o0[id.xy]=l; o1[id.xy/2]=l; o2[id.xy/4]=l; o3[id.xy/8]=l; }");
    // generated frame: (prev + cur) / 2 with the flow bound; both inputs carry the same pattern, so the result is the pattern
    ID3D11ComputeShader* csGen = MakeCS(dev, "Texture2D<float4> a:register(t0); Texture2D<float4> b:register(t1); Texture2D<float4> fl:register(t2); RWTexture2D<float4> o:register(u0); [numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){ o[id.xy]=(a[id.xy]+b[id.xy])*0.5+fl[id.xy/4].x*0; }");
    if (!csPyr || !csGen) return 1;
    // LSFG flow as seen in LS 3.x: after the pyramid pass, RGBA16F flow levels with the coarser level bound at S4.
    // Constant (+4, 0) units = 8 flow px to the left in the previous frame; zw = the reverse field.
    const UINT FLW = W / 4, FLH = H / 4;
    ID3D11Texture2D* flow16 = MakeTex(dev, FLW, FLH, DXGI_FORMAT_R16G16B16A16_FLOAT, true); ID3D11UnorderedAccessView* uFlow16 = uav(flow16); ID3D11ShaderResourceView* sPyr3 = srv(pyr[3]), * sFlow16 = srv(flow16);
    const std::string flowSrc = flowSplit
        ? "Texture2D<float> c:register(t4); RWTexture2D<float4> o:register(u0); [numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){ o[id.xy]=(id.x<" + std::to_string(FLW / 2) + " ? float4(4,0,-4,0) : float4(4,0,4,0))+c[id.xy/8].x*0; }"
        : std::string("Texture2D<float> c:register(t4); RWTexture2D<float4> o:register(u0); [numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){ o[id.xy]=float4(4,0,-4,0)+c[id.xy/8].x*0; }");
    ID3D11ComputeShader* csFlow16 = MakeCS(dev, flowSrc.c_str());
    if (!flow16 || !uFlow16 || !sPyr3 || !sFlow16 || !csFlow16) { printf("flow16 setup failed\n"); return 1; }

    uint64_t presents = 0, composedSeen = 0, checks = 0; double lastMean = 0;
    // Before writing the back buffer: with FLIP_SEQUENTIAL and two buffers it still holds what was presented two
    // presents ago (pattern + whatever the addon composed on top). Every present's raw content is the pattern.
    auto checkBackbuffer = [&](const char* bmp) {
        ID3D11Texture2D* bb = nullptr; sc->GetBuffer(0, IID_PPV_ARGS(&bb)); if (!bb) return;
        double mean = 0; uint64_t changed = CountChanged(dev, dc, bb, W, H, &mean, bmp); bb->Release();
        // counted from the first present that carries a result: the model's first build (NGX's, cold, up to a second) is not the compose's fault
        const bool composed = changed > (uint64_t)W * H / 20;
        if (!composed && !composedSeen) return;
        checks++; if (composed) { composedSeen++; lastMean = mean; }
    };
    auto present = [&](bool real) {
        ID3D11Texture2D* bb = nullptr; sc->GetBuffer(0, IID_PPV_ARGS(&bb));
        if (!barsMode && presents >= 2 && presents % 7 == 0) checkBackbuffer(presents == 210 ? (real ? "present_real.bmp" : "present_gen.bmp") : nullptr);
        if (barsMode) {   // black all over, the frame in the middle
            ID3D11RenderTargetView* rtv = nullptr; dev->CreateRenderTargetView(bb, nullptr, &rtv);
            if (rtv) { const float black[4] = { 0, 0, 0, 1 }; dc->ClearRenderTargetView(rtv, black); rtv->Release(); }
        }
        if (real) { if (barsMode) dc->CopySubresourceRegion(bb, 0, (SW - W) / 2, 0, 0, cur, 0, nullptr); else dc->CopyResource(bb, cur); }
        else {
            ID3D11ShaderResourceView* s3[3] = { sPrev, sCur, sFlow16 }; dc->CSSetShaderResources(0, 3, s3); dc->CSSetUnorderedAccessViews(0, 1, &uOut, nullptr); dc->CSSetShader(csGen, nullptr, 0); host.Dispatch(dc, W / 8, H / 8, 1);
            dc->CSSetUnorderedAccessViews(0, 1, nullu, nullptr); dc->CSSetShaderResources(0, 3, nulls);
            if (barsMode) dc->CopySubresourceRegion(bb, 0, (SW - W) / 2, 0, 0, out, 0, nullptr); else dc->CopyResource(bb, out);
        }
        bb->Release();
        void* before = (*(void***)sc)[8];
        sc->Present(0, 0); presents++; pump();
        void* after = (*(void***)sc)[8];
        if (presents <= 3 || before != after) printf("[hosttest] present #%llu: vtable Present %p -> %p%s\n", (unsigned long long)presents, before, after, before != after ? " (CHANGED)" : "");
    };
    for (int fr = 0; fr < 90; ++fr) {
        Fill(dc, prev, W, H); Fill(dc, cur, W, H);   // fresh capture every real frame, like LS
        // per real frame: pyramid pass on the new frame (the TAP)
        dc->CSSetShaderResources(0, 1, &sCur); dc->CSSetUnorderedAccessViews(0, 4, uPyr, nullptr); dc->CSSetShader(csPyr, nullptr, 0); host.Dispatch(dc, W * 7 / 10 / 8, H * 7 / 10 / 8, 1);
        ID3D11UnorderedAccessView* null4[4] = {}; dc->CSSetUnorderedAccessViews(0, 4, null4, nullptr); dc->CSSetShaderResources(0, 3, nulls);
        // low-res flow-ish pass
        ID3D11ShaderResourceView* s1[2] = { sPrev, sCur }; dc->CSSetShaderResources(0, 2, s1); dc->CSSetUnorderedAccessViews(0, 1, &uFlow, nullptr); dc->CSSetShader(csFlow, nullptr, 0); host.Dispatch(dc, FW / 8, FH / 8, 1);
        dc->CSSetUnorderedAccessViews(0, 1, nullu, nullptr); dc->CSSetShaderResources(0, 3, nulls);
        // finest LSFG flow level (RGBA16F UAV0, coarser level at S4): what the addon feeds the model as motion vectors
        dc->CSSetShaderResources(4, 1, &sPyr3); dc->CSSetUnorderedAccessViews(0, 1, &uFlow16, nullptr); dc->CSSetShader(csFlow16, nullptr, 0); host.Dispatch(dc, FLW / 8, FLH / 8, 1);
        { ID3D11ShaderResourceView* n8[8] = {}; dc->CSSetShaderResources(0, 8, n8); } dc->CSSetUnorderedAccessViews(0, 1, nullu, nullptr);
        // X3: generated, generated, real
        present(false); std::this_thread::sleep_for(std::chrono::milliseconds(10));
        present(false); std::this_thread::sleep_for(std::chrono::milliseconds(10));
        present(true);
        if (fr % 15 == 0) frame("dispatching"); else emptyFrame();
        std::swap(sPrev, sCur); std::this_thread::sleep_for(std::chrono::milliseconds(13));
    }
    dc->Flush(); std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // nis=1: Lossless Scaling's NIS pass as it looks (the frame, NIS's two 2x64 RGBA32F coefficient tables, a 1.5x output, one thread group
    // per 32x24 pixels), twice per real frame as with frame generation 2x. This fake pass paints its output magenta: the DLSS upscaler must
    // take its place and write a real upscaled picture instead.
    bool nisMode = false;
    for (int i = 4; i < argc; ++i) if (!strcmp(argv[i], "nis=1")) nisMode = true;
    if (nisMode) {
        UINT NW = W, NH = H; double NS = 1.5;   // nisW= nisH= nisScale=: the frame's size and the scale (1 = DLAA), e.g. 3840x2160 at 1
        for (int i = 4; i < argc; ++i) { if (!strncmp(argv[i], "nisW=", 5)) NW = (UINT)atoi(argv[i] + 5); if (!strncmp(argv[i], "nisH=", 5)) NH = (UINT)atoi(argv[i] + 5);
                                         if (!strncmp(argv[i], "nisScale=", 9)) NS = atof(argv[i] + 9); }
        // nisvp=1: a window of another shape than the screen (4:3 on 16:9): NIS scales the frame into a centred part of a 16:9 output (its
        // output viewport, in its constants, NISConfig) and dispatches over that part only; the rest of the output (the borders) stays black
        bool nisVp = false; for (int i = 4; i < argc; ++i) if (!strcmp(argv[i], "nisvp=1")) nisVp = true;
        const UINT VW = (UINT)(NW * NS + 0.5), VH = (UINT)(NH * NS + 0.5);
        const UINT OW = nisVp ? std::max(VW, (VH * 16 + 8) / 9) : VW, OH = VH;
        const UINT OX = (OW - VW) / 2, OY = 0;
        bool nisBgra = false;   // nisbgra=1: the frame in BGRA8, as Lossless Scaling hands it to NIS with frame generation off
        for (int i = 4; i < argc; ++i) if (!strcmp(argv[i], "nisbgra=1")) nisBgra = true;
        ID3D11Texture2D* nisIn = MakeTex(dev, NW, NH, nisBgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM, false);
        ID3D11Texture2D* coef1 = MakeTex(dev, 2, 64, DXGI_FORMAT_R32G32B32A32_FLOAT, false), * coef2 = MakeTex(dev, 2, 64, DXGI_FORMAT_R32G32B32A32_FLOAT, false);
        ID3D11Texture2D* nisOut = MakeTex(dev, OW, OH, DXGI_FORMAT_R8G8B8A8_UNORM, true);
        ID3D11ShaderResourceView* nisSrvs[3] = { srv(nisIn), srv(coef1), srv(coef2) }; ID3D11UnorderedAccessView* uNisOut = uav(nisOut);
        // nisgen=1: as in Lossless Scaling, the real frame's NIS pass reads the very frame the capture pass (the TAP) read, and a generated frame's reads a texture of its own (the tap's frame identity tells them apart)
        bool genTex = false; for (int i = 4; i < argc; ++i) if (!strcmp(argv[i], "nisgen=1")) genTex = true;
        ID3D11Texture2D* nisInGen = genTex ? MakeTex(dev, NW, NH, nisBgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM, false) : nullptr;
        ID3D11ShaderResourceView* sNisGen = nisInGen ? srv(nisInGen) : nullptr;
        { const float black[4] = {}; dc->ClearUnorderedAccessViewFloat(uNisOut, black); }
        // NIS's constants as NISConfig lays them out: 18 floats (kScaleX, kScaleY at 12 and 13), then the input and output viewports
        uint32_t nisCfg[28] = {};
        { const float sx = (float)NW / VW, sy = (float)NH / VH; memcpy(&nisCfg[12], &sx, 4); memcpy(&nisCfg[13], &sy, 4);
          const uint32_t vp[8] = { 0, 0, NW, NH, OX, OY, VW, VH }; memcpy(&nisCfg[18], vp, sizeof vp); }
        ID3D11Buffer* nisCb = nullptr;
        { D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof nisCfg; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;   // as a pass that updates it each frame
          bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
          D3D11_SUBRESOURCE_DATA sd{ nisCfg }; dev->CreateBuffer(&bd, &sd, &nisCb); }
        ID3D11ComputeShader* csNis = MakeCS(dev, "Texture2D<float4> f:register(t0); Texture2D<float4> c1:register(t1); Texture2D<float4> c2:register(t2); RWTexture2D<float4> o:register(u0);"
                                                 " cbuffer c:register(b0){ uint4 cfg[7]; };"
                                                 " [numthreads(32,24,1)] void main(uint3 id:SV_DispatchThreadID){ if (id.x < cfg[6].x && id.y < cfg[6].y)"
                                                 " o[id.xy + cfg[5].zw]=float4(1,0,1,1)+(f[uint2(0,0)]+c1[uint2(0,0)]+c2[uint2(0,0)])*0; }");
        Fill(dc, nisIn, NW, NH);
        if (nisInGen) Fill(dc, nisInGen, NW, NH);
        // nisedge=1: the frame is a hard slanted edge drawn without anti-aliasing (white where u - 3v passes the middle, point-sampled: stair
        // steps three pixels long), and the output is measured against the ideal smooth edge ([check-edge])
        bool nisEdge = false; for (int i = 4; i < argc; ++i) if (!strcmp(argv[i], "nisedge=1")) nisEdge = true;
        auto edgeDistance = [&](double u, double v) { return ((u - NW * 0.5) - 3.0 * (v - NH * 0.5)) / std::sqrt(10.0); };   // in frame pixels
        if (nisEdge) {
            std::vector<uint32_t> px((size_t)NW * NH);
            for (UINT y = 0; y < NH; ++y) for (UINT x = 0; x < NW; ++x) px[(size_t)y * NW + x] = edgeDistance(x + 0.5, y + 0.5) > 0.0 ? 0xFFFFFFFFu : 0xFF000000u;
            dc->UpdateSubresource(nisIn, 0, nullptr, px.data(), NW * 4, 0);
        }
        // nisline=1: a thin bright line swaying sideways (as a wire in the wind) over a still background: its own motion, unlike the blocks
        // around it. The output is measured against the true picture near the line ([check-line])
        bool nisLine = false; for (int i = 4; i < argc; ++i) if (!strcmp(argv[i], "nisline=1")) nisLine = true;
        auto lineX = [&](double v, int fr) { return NW * 0.5 + (v - NH * 0.5) * 0.35 + 2.5 * std::sin(fr * 0.7); };   // in frame pixels
        auto linePixel = [&](UINT x, UINT y, int fr) { return std::abs(x + 0.5 - lineX(y + 0.5, fr)) < 0.6 ? 0xFFFFFFFFu : Pattern(x, y, NW, NH); };
        std::vector<uint32_t> linePx;
        auto fillLine = [&](int fr) {
            linePx.resize((size_t)NW * NH);
            for (UINT y = 0; y < NH; ++y) for (UINT x = 0; x < NW; ++x) linePx[(size_t)y * NW + x] = linePixel(x, y, fr);
            dc->UpdateSubresource(nisIn, 0, nullptr, linePx.data(), NW * 4, 0);
        };
        // gpuload=<n>: before each NIS pass, n thousand iterations of busy work on this (Lossless Scaling's) queue, as a game at its GPU
        // limit keeps the card busy; the upscaler's frame read and signal then queue behind it (Fallout: New Vegas, 2026-09-25)
        int gpuLoad = 0; for (int i = 4; i < argc; ++i) if (!strncmp(argv[i], "gpuload=", 8)) gpuLoad = std::clamp(atoi(argv[i] + 8), 0, 1000);
        ID3D11ComputeShader* csLoad = nullptr; ID3D11Texture2D* loadTex = nullptr; ID3D11UnorderedAccessView* uLoad = nullptr;
        if (gpuLoad) {
            const std::string src = "RWTexture2D<float4> o:register(u0); [numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){ float4 v=float4(id.xyxy)*0.001;"
                                    " [loop] for(uint i=0;i<" + std::to_string(gpuLoad * 1000) + "u;++i) v=sin(v*1.0001+0.1); o[id.xy]=v; }";
            csLoad = MakeCS(dev, src.c_str()); loadTex = MakeTex(dev, 256, 256, DXGI_FORMAT_R16G16B16A16_FLOAT, true); uLoad = uav(loadTex);
        }
        auto nisPass = [&](bool real = true) {
            if (csLoad) {   // the game's frame, on the same GPU
                dc->CSSetUnorderedAccessViews(0, 1, &uLoad, nullptr); dc->CSSetShader(csLoad, nullptr, 0); dc->Dispatch(32, 32, 1);
                dc->CSSetUnorderedAccessViews(0, 1, nullu, nullptr);
            }
            ID3D11ShaderResourceView* passSrvs[3] = { !real && sNisGen ? sNisGen : nisSrvs[0], nisSrvs[1], nisSrvs[2] };
            dc->CSSetShaderResources(0, 3, passSrvs); dc->CSSetUnorderedAccessViews(0, 1, &uNisOut, nullptr); dc->CSSetShader(csNis, nullptr, 0);
            dc->CSSetConstantBuffers(0, 1, &nisCb);
            host.Dispatch(dc, (VW + 31) / 32, (VH + 23) / 24, 1);
            ID3D11Buffer* noCb = nullptr; dc->CSSetConstantBuffers(0, 1, &noCb);
            dc->CSSetUnorderedAccessViews(0, 1, nullu, nullptr); dc->CSSetShaderResources(0, 3, nulls);
        };
        bool nisGen = false;   // nisgen=1: a generated-frame pass (two full frames and the flow in, a full frame out) before the first of the two NIS passes, as LSFG draws it
        bool nisNoFlow = false, nisMove = false;   // nisnoflow=1: frame generation off, so no capture or flow passes, only NIS; nismove=1: the picture slides
        for (int i = 4; i < argc; ++i) { if (!strcmp(argv[i], "nisnoflow=1")) nisNoFlow = true; if (!strcmp(argv[i], "nismove=1")) nisMove = true; if (!strcmp(argv[i], "nisgen=1")) nisGen = true; }
        // nisswitch=<key>=<value>: that setting changes at frame 75, and the run goes on for 325 frames more, so a runtime that starts
        // again cold (DLSS: two seconds or more) still has passes to replace (as the manager's Runtimes list changes "fsrRuntime" while the game runs)
        std::string switchKey, switchValue;
        for (int i = 4; i < argc; ++i) if (!strncmp(argv[i], "nisswitch=", 10)) {
            const char* kv = argv[i] + 10; const char* eq = strchr(kv, '=');
            if (eq) { switchKey.assign(kv, eq - kv); switchValue = eq + 1; }
        }
        const int kFrames = switchKey.empty() ? (nisNoFlow ? 260 : 150) : 400, kSwitchAt = 75;   // (no flow: frames are 16 ms apart, and the engine takes 2 to 2.7 s to start: a run of 150 ended before it did on a busy machine)
        int nisGap = 12; for (int i = 4; i < argc; ++i) if (!strncmp(argv[i], "nisgap=", 7)) nisGap = std::clamp(atoi(argv[i] + 7), 0, 24);
        for (int fr = 0; fr < kFrames; ++fr) {
            if (fr == kSwitchAt && !switchKey.empty()) { host.cfg[switchKey] = switchValue; printf("[hosttest] frame %d: %s = %s\n", fr, switchKey.c_str(), switchValue.c_str()); }
            if (nisMove) FillMoving(dc, nisIn, NW, NH, fr);
            if (nisLine) fillLine(fr);
            if (nisNoFlow) { nisPass(); std::this_thread::sleep_for(std::chrono::milliseconds(16)); if (fr % 30 == 0) frame("nis"); else emptyFrame(); continue; }
            Fill(dc, cur, W, H);
            dc->CSSetShaderResources(0, 1, genTex ? &nisSrvs[0] : &sCur); dc->CSSetUnorderedAccessViews(0, 4, uPyr, nullptr); dc->CSSetShader(csPyr, nullptr, 0); host.Dispatch(dc, W * 7 / 10 / 8, H * 7 / 10 / 8, 1);
            ID3D11UnorderedAccessView* null4[4] = {}; dc->CSSetUnorderedAccessViews(0, 4, null4, nullptr); dc->CSSetShaderResources(0, 3, nulls);
            dc->CSSetShaderResources(4, 1, &sPyr3); dc->CSSetUnorderedAccessViews(0, 1, &uFlow16, nullptr); dc->CSSetShader(csFlow16, nullptr, 0); host.Dispatch(dc, FLW / 8, FLH / 8, 1);
            { ID3D11ShaderResourceView* n8[8] = {}; dc->CSSetShaderResources(0, 8, n8); } dc->CSSetUnorderedAccessViews(0, 1, nullu, nullptr);
            if (nisGen) { ID3D11ShaderResourceView* s3[3] = { sPrev, sCur, sFlow16 }; dc->CSSetShaderResources(0, 3, s3); dc->CSSetUnorderedAccessViews(0, 1, &uOut, nullptr); dc->CSSetShader(csGen, nullptr, 0); host.Dispatch(dc, W / 8, H / 8, 1);
                          dc->CSSetShaderResources(0, 3, nulls); dc->CSSetUnorderedAccessViews(0, 1, nullu, nullptr); }
            nisPass(false); std::this_thread::sleep_for(std::chrono::milliseconds(nisGap));   // the generated frame (nisgap=: how soon the real one follows)
            nisPass(); std::this_thread::sleep_for(std::chrono::milliseconds(24 - nisGap));   // the real one
            if (fr % 30 == 0) frame("nis"); else emptyFrame();
        }
        dc->Flush(); std::this_thread::sleep_for(std::chrono::milliseconds(300));
        // read the output back: how much of it is the fake pass's magenta, and how close its average colour is to the frame's
        D3D11_TEXTURE2D_DESC sd{}; nisOut->GetDesc(&sd); sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* st = nullptr; dev->CreateTexture2D(&sd, nullptr, &st);
        uint64_t magenta = 0, borderLit = 0, edgePixels = 0, linePixels = 0; double sum[3] = {}, want[3] = {}, detail = 0, edgeError = 0, lineError[3] = {};   // detail: the average step between neighbouring pixels (sharpening raises it)
        double moveError[3] = {};   // nismove: how far the picture is from the moving picture of the last three frames (the one shown is a frame late)
        if (st) {
            dc->CopyResource(st, nisOut);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(dc->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                for (UINT oy = 0; oy < OH; ++oy) for (UINT ox = 0; ox < OW; ++ox) {
                    const uint8_t* px = static_cast<const uint8_t*>(m.pData) + oy * m.RowPitch + ox * 4;
                    const bool inside = ox >= OX && ox < OX + VW && oy >= OY && oy < OY + VH;
                    if (!inside) { if (px[0] | px[1] | px[2]) ++borderLit; continue; }   // Lossless Scaling's borders: nothing may be drawn there
                    const UINT x = ox - OX, y = oy - OY;   // within the viewport, as NIS's picture
                    if (px[0] == 255 && px[1] == 0 && px[2] == 255) ++magenta;
                    if (nisLine) {   // near the line (within 4 frame pixels of it in any of the last three frames): off the true picture of each
                        const double u = (x + 0.5) / NS, v = (y + 0.5) / NS;
                        bool nearLine = false;
                        for (int k = 0; k < 3; ++k) nearLine = nearLine || std::abs(u - lineX(v, kFrames - 1 - k)) < 4.0;
                        if (nearLine && y > 8 && y + 8 < VH) {
                            ++linePixels;
                            for (int k = 0; k < 3; ++k) {
                                const uint32_t t = linePixel(std::min((UINT)u, NW - 1), std::min((UINT)v, NH - 1), kFrames - 1 - k);   // B, G, R in memory; nisbgra
                                lineError[k] += (std::abs(int(px[0]) - int((t >> 16) & 255)) + std::abs(int(px[1]) - int((t >> 8) & 255)) + std::abs(int(px[2]) - int(t & 255))) / 3.0;
                            }
                        }
                    }
                    if (nisEdge) {   // near the edge: how far from the ideal (a box filter one frame pixel wide over the true edge)
                        const double d = edgeDistance((x + 0.5) / NS, (y + 0.5) / NS);
                        if (std::abs(d) < 3.0 && x > 8 && y > 8 && x + 8 < VW && y + 8 < VH) {
                            const double ideal = 255.0 * std::clamp(0.5 + d, 0.0, 1.0);
                            edgeError += std::abs(px[0] - ideal); ++edgePixels;
                        }
                    }
                    for (int c = 0; c < 3; ++c) sum[c] += px[c];
                    if (x + 1 < VW) detail += std::abs(int(px[4]) - int(px[0])) + std::abs(int(px[5]) - int(px[1])) + std::abs(int(px[6]) - int(px[2]));
                    if (nisMove && x >= VW / 8 && x < VW * 7 / 8 && y >= VH / 8 && y < VH * 7 / 8)   // the middle (edges have no frame before to come from)
                        for (int k = 0; k < 3; ++k) {
                            const uint32_t v = Ideal((int)x, (int)y, NS, kFrames - 1 - k);   // the output is RGBA8: R, G, B
                            moveError[k] += std::abs(int(px[0]) - int((v >> 16) & 255)) + std::abs(int(px[1]) - int((v >> 8) & 255)) + std::abs(int(px[2]) - int(v & 255));
                        }
                }
                dc->Unmap(st, 0);
            }
            st->Release();
        }
        if (nisMove) {
            const double n = 3.0 * (VW * 3 / 4) * (VH * 3 / 4);
            int shown = 0; for (int k = 1; k < 3; ++k) if (moveError[k] < moveError[shown]) shown = k;
            printf("[check-move] the picture is off the moving picture by %.2f levels a channel (frame shown: %d before the last; the others %.2f, %.2f, %.2f)\n",
                   moveError[shown] / n, shown, moveError[0] / n, moveError[1] / n, moveError[2] / n);
        }
        if (nisLine && linePixels) {
            int shown = 0; for (int k = 1; k < 3; ++k) if (lineError[k] < lineError[shown]) shown = k;
            printf("[check-line] near the swaying line the picture is off the true one by %.2f levels over %llu pixels (frame shown: %d before the last)\n",
                   lineError[shown] / linePixels, (unsigned long long)linePixels, shown);
        }
        if (nisEdge) printf("[check-edge] the slanted edge is off the ideal smooth edge by %.2f levels over %llu pixels\n", edgePixels ? edgeError / edgePixels : 0.0,
                            (unsigned long long)edgePixels);
        for (UINT y = 0; y < NH; ++y) for (UINT x = 0; x < NW; ++x) {
            const uint32_t v = nisLine ? linePixel(x, y, kFrames - 1) : nisEdge ? (edgeDistance(x + 0.5, y + 0.5) > 0.0 ? 0xFFFFFFFFu : 0xFF000000u)
                             : nisMove ? MovingPixel((int)x, (int)y, kFrames - 1) : Pattern(x, y, NW, NH);   // in memory: B, G, R
            if (nisBgra) { want[0] += (v >> 16) & 255; want[1] += (v >> 8) & 255; want[2] += v & 255; }   // the RGBA8 output holds R, G, B
            else { want[0] += v & 255; want[1] += (v >> 8) & 255; want[2] += (v >> 16) & 255; }
        }
        double worst = 0;
        for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(sum[c] / (double(VW) * VH) - want[c] / (double(NW) * NH)));
        const bool replaced = magenta < (uint64_t)VW * VH / 100 && worst < 6.0;
        printf("[check-nis] %ux%u -> %ux%u: %.2f%% of the output is the fake NIS pass's magenta, average colour off by %.2f levels, detail %.3f (%s)\n", NW, NH, VW, VH,
               100.0 * magenta / (double(VW) * VH), worst, detail / (3.0 * (VW - 1) * VH), replaced ? "DLSS REPLACED NIS" : "NIS KEPT");
        if (nisVp) printf("[check-vp] %ux%u at %u,%u of a %ux%u output: %llu pixels of the borders lit (%s)\n", VW, VH, OX, OY, OW, OH, (unsigned long long)borderLit,
                          borderLit == 0 ? "BORDERS KEPT" : "BORDERS DRAWN OVER");
        for (auto* v : nisSrvs) v->Release();
        if (nisCb) nisCb->Release();
        // nishdr=scrgb|pq: then the same NIS pass on an HDR frame (16-bit float scRGB, or 10-bit HDR10): hdr='s picture, with its band of
        // highlights at 1000 nits across the middle. The upscaler must replace NIS there too, write no NaN, and keep the highlights and the
        // picture's brightness: it works on the frame's SDR view and puts the picture back in the frame's own encoding ([check-nishdr]).
        int nisHdr = 0;
        for (int i = 4; i < argc; ++i) { if (!strcmp(argv[i], "nishdr=scrgb")) nisHdr = 1; if (!strcmp(argv[i], "nishdr=pq")) nisHdr = 2; }
        if (nisHdr && !nisVp) {
            const bool pq = nisHdr == 2;
            ID3D11Texture2D* hIn = MakeHdrPicture(dev, dc, NW, NH, pq);
            ID3D11Texture2D* hOut = MakeTex(dev, VW, VH, pq ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT, true);
            if (!hIn || !hOut) printf("[check-nishdr] the HDR textures could not be made\n");
            else {
                ID3D11ShaderResourceView* hSrvs[3] = { srv(hIn), srv(coef1), srv(coef2) }; ID3D11UnorderedAccessView* uHOut = uav(hOut);
                for (int fr = 0; fr < 120; ++fr) {
                    dc->CSSetShaderResources(0, 3, hSrvs); dc->CSSetUnorderedAccessViews(0, 1, &uHOut, nullptr); dc->CSSetShader(csNis, nullptr, 0);
                    dc->CSSetConstantBuffers(0, 1, &nisCb);
                    host.Dispatch(dc, (VW + 31) / 32, (VH + 23) / 24, 1);
                    ID3D11Buffer* noCb = nullptr; dc->CSSetConstantBuffers(0, 1, &noCb);
                    dc->CSSetUnorderedAccessViews(0, 1, nullu, nullptr); dc->CSSetShaderResources(0, 3, nulls);
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    if (fr % 30 == 0) frame("nishdr"); else emptyFrame();
                }
                dc->Flush(); std::this_thread::sleep_for(std::chrono::milliseconds(300));
                // read back: magenta (the fake pass), NaN, the band's core against its value, the rest's mean against the picture's
                D3D11_TEXTURE2D_DESC sd{}; hOut->GetDesc(&sd); sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ID3D11Texture2D* st = nullptr; dev->CreateTexture2D(&sd, nullptr, &st);
                uint64_t magenta = 0, bad = 0, core = 0, rest = 0; double coreSum = 0, restSum = 0, restWant = 0;
                if (st) {
                    dc->CopyResource(st, hOut); D3D11_MAPPED_SUBRESOURCE m{};
                    if (SUCCEEDED(dc->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                        for (UINT y = 0; y < VH; ++y) for (UINT x = 0; x < VW; ++x) {
                            float got[3], want[3]; HdrPixel(x, y, VW, VH, pq, want);
                            const char* row = (const char*)m.pData + y * m.RowPitch;
                            if (pq) { const uint32_t v = reinterpret_cast<const uint32_t*>(row)[x]; for (int i = 0; i < 3; ++i) got[i] = ((v >> (10 * i)) & 1023) / 1023.0f; }
                            else { using namespace DirectX::PackedVector; const uint16_t* v = reinterpret_cast<const uint16_t*>(row) + x * 4; for (int i = 0; i < 3; ++i) got[i] = XMConvertHalfToFloat(v[i]); }
                            if (!std::isfinite(got[0]) || !std::isfinite(got[1]) || !std::isfinite(got[2])) { ++bad; continue; }
                            if (fabsf(got[0] - 1.0f) < 0.01f && got[1] < 0.01f && fabsf(got[2] - 1.0f) < 0.01f) ++magenta;
                            const float mean = (got[0] + got[1] + got[2]) / 3.0f, wantMean = (want[0] + want[1] + want[2]) / 3.0f;
                            if (y >= VH * 47 / 100 && y < VH * 53 / 100) { ++core; coreSum += mean; }   // the band, away from its edges
                            else if (y < VH * 43 / 100 || y >= VH * 57 / 100) { ++rest; restSum += mean; restWant += wantMean; }
                        }
                        dc->Unmap(st, 0);
                    }
                    st->Release();
                }
                float bandWant[3]; HdrPixel(0, VH / 2, VW, VH, pq, bandWant);
                const double coreMean = core ? coreSum / core : 0, restMean = rest ? restSum / rest : 0, restWantMean = rest ? restWant / rest : 0;
                const bool replaced = magenta < (uint64_t)VW * VH / 100;
                const bool bright = bandWant[0] > 0 && std::abs(coreMean - bandWant[0]) < 0.03 * bandWant[0];
                const bool level = restWantMean > 0 && std::abs(restMean - restWantMean) < 0.05 * restWantMean;
                printf("[check-nishdr] %s %ux%u -> %ux%u: %.2f%% magenta (%s), %llu NaN, the highlights %.4f against %.4f (%s), the rest's mean %.4f against %.4f (%s)\n",
                       pq ? "HDR10" : "scRGB", NW, NH, VW, VH, 100.0 * magenta / (double(VW) * VH), replaced ? "HDR REPLACED NIS" : "HDR NIS KEPT", (unsigned long long)bad,
                       coreMean, bandWant[0], bright ? "HIGHLIGHTS KEPT" : "HIGHLIGHTS LOST", restMean, restWantMean, level ? "LEVEL KEPT" : "LEVEL OFF");
                uHOut->Release(); for (auto* v : hSrvs) v->Release();
            }
            if (hIn) hIn->Release(); if (hOut) hOut->Release();
        }
        uNisOut->Release(); csNis->Release(); nisIn->Release(); if (nisInGen) nisInGen->Release(); coef1->Release(); coef2->Release(); nisOut->Release();
    }
    {   // the tap must be read-only now: LS's frame textures keep the original pattern
        for (ID3D11Texture2D* t : { prev, cur }) { double mean = 0; uint64_t changed = CountChanged(dev, dc, t, W, H, &mean, nullptr);
            printf("[check] frame texture %p: %llu / %llu pixels differ from the pattern (%s)\n", (void*)t, (unsigned long long)changed, (unsigned long long)W * H, changed == 0 ? "TAP READ-ONLY" : "TAP WROTE INTO LS'S FRAME"); }
        printf("[check] presented buffers: %llu of %llu sampled presents carried the model's delta (mean abs diff of changed px %.2f) -> %s\n", (unsigned long long)composedSeen, (unsigned long long)checks, lastMean, composedSeen >= checks / 2 ? "COMPOSE APPLIED" : "COMPOSE MISSING");
    }
    for (int i = 0; i < 3; ++i) frame("after");
    printf("[check] live metrics: frame_ms x%d (last %.1f), model_ms x%d (last %.1f), keepup_pct x%d (last %.0f), status '%s' (level %d, %d updates) -> %s\n",
           host.metricCount["frame_ms"], host.metricLast["frame_ms"], host.metricCount["model_ms"], host.metricLast["model_ms"], host.metricCount["keepup_pct"], host.metricLast["keepup_pct"],
           host.status.c_str(), host.statusLevel, host.statusCalls,
           (host.metricCount["frame_ms"] > 20 && host.metricCount["model_ms"] > 2 && host.metricCount["keepup_pct"] > 2 && host.statusCalls > 0 && host.statusLevel >= 1) ? "LIVE METRICS OK" : "LIVE METRICS MISSING");
    if (shotMode) { shot.Frame(panel, 4); printf("[shot] %s (%dx%d): %s\n", shotPath.c_str(), shotW, shotH, shot.Save(shotPath.c_str()) ? "written" : "FAILED"); }

    // ---- resolution change (fullscreen toggle): the old shapes stay in the table, the tap must follow the new frame size
    {
        const UINT W2 = 1600, H2 = 900;
        ID3D11Texture2D* c2 = MakeTex(dev, W2, H2, DXGI_FORMAT_B8G8R8A8_UNORM, true), * p2c = MakeTex(dev, W2, H2, DXGI_FORMAT_B8G8R8A8_UNORM, true), * o2 = MakeTex(dev, W2, H2, DXGI_FORMAT_B8G8R8A8_UNORM, true);
        ID3D11ShaderResourceView* sC2 = srv(c2), * sP2 = srv(p2c); ID3D11UnorderedAccessView* uO2 = uav(o2);
        ID3D11Texture2D* p2[4]; ID3D11UnorderedAccessView* uP2[4];
        for (int i = 0; i < 4; ++i) { UINT pw = (W2 * 7 / 10) >> i, ph = (H2 * 7 / 10) >> i; p2[i] = MakeTex(dev, pw, ph, DXGI_FORMAT_R8_UNORM, true); uP2[i] = uav(p2[i]); }
        ID3D11Texture2D* la = MakeTex(dev, FW, FH, DXGI_FORMAT_B8G8R8A8_UNORM, true), * lb = MakeTex(dev, FW, FH, DXGI_FORMAT_B8G8R8A8_UNORM, true);   // low-res inputs for the filler pass (old frames are gone after a real mode switch)
        ID3D11ShaderResourceView* sLa = srv(la), * sLb = srv(lb);
        sc->ResizeBuffers(2, W2, H2, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        host.longestCallMs = 0;   // from here on the model is made again for the new size: that must not hold up the render thread
        uint64_t checks2 = 0, composed2 = 0;
        for (int fr = 0; fr < 150; ++fr) {
            Fill(dc, c2, W2, H2); Fill(dc, p2c, W2, H2);
            dc->CSSetShaderResources(0, 1, &sC2); dc->CSSetUnorderedAccessViews(0, 4, uP2, nullptr); dc->CSSetShader(csPyr, nullptr, 0); host.Dispatch(dc, W2 * 7 / 10 / 8, H2 * 7 / 10 / 8, 1);
            ID3D11UnorderedAccessView* null4[4] = {}; dc->CSSetUnorderedAccessViews(0, 4, null4, nullptr); dc->CSSetShaderResources(0, 3, nulls);
            // this frame's finest flow pass, which the tap waits for before it hands the frame over
            dc->CSSetShaderResources(4, 1, &sPyr3); dc->CSSetUnorderedAccessViews(0, 1, &uFlow16, nullptr); dc->CSSetShader(csFlow16, nullptr, 0); host.Dispatch(dc, FLW / 8, FLH / 8, 1);
            { ID3D11ShaderResourceView* n8[8] = {}; dc->CSSetShaderResources(0, 8, n8); } dc->CSSetUnorderedAccessViews(0, 1, nullu, nullptr);
            for (int k = 0; k < 3; ++k) {
                ID3D11Texture2D* bb = nullptr; sc->GetBuffer(0, IID_PPV_ARGS(&bb));
                if (fr > 100 && fr % 10 == 0 && k == 0) { double mean = 0; uint64_t changed = CountChanged(dev, dc, bb, W2, H2, &mean, nullptr); checks2++; if (changed > (uint64_t)W2 * H2 / 20) composed2++; }
                if (k == 2) dc->CopyResource(bb, c2);
                else { ID3D11ShaderResourceView* s3[3] = { sP2, sC2, sFlow16 }; dc->CSSetShaderResources(0, 3, s3); dc->CSSetUnorderedAccessViews(0, 1, &uO2, nullptr); dc->CSSetShader(csGen, nullptr, 0); host.Dispatch(dc, W2 / 8, H2 / 8, 1); dc->CSSetUnorderedAccessViews(0, 1, nullu, nullptr); dc->CSSetShaderResources(0, 3, nulls); dc->CopyResource(bb, o2); }
                bb->Release(); sc->Present(0, 0); pump();
            }
            // the low-res passes keep hammering the table so the stale detector has dispatches to count
            for (int k = 0; k < 40; ++k) { ID3D11ShaderResourceView* s1[2] = { sLa, sLb }; dc->CSSetShaderResources(0, 2, s1); dc->CSSetUnorderedAccessViews(0, 1, &uFlow, nullptr); dc->CSSetShader(csFlow, nullptr, 0); host.Dispatch(dc, FW / 8, FH / 8, 1); dc->CSSetUnorderedAccessViews(0, 1, nullu, nullptr); dc->CSSetShaderResources(0, 3, nulls); }
            if (fr % 30 == 0) frame("res2"); else emptyFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        dc->Flush(); std::this_thread::sleep_for(std::chrono::milliseconds(300));
        double mean = 0; uint64_t changed = CountChanged(dev, dc, c2, W2, H2, &mean, nullptr);
        printf("[check-res2] %ux%u frame texture: %llu pixels changed (%s); presented buffers composed in %llu of %llu samples (%s)\n", W2, H2, (unsigned long long)changed, changed == 0 ? "read-only" : "WRITTEN", (unsigned long long)composed2, (unsigned long long)checks2, composed2 >= checks2 / 2 && checks2 ? "TAP FOLLOWED" : "TAP LOST");
        printf("[check-res2] the longest the addon held up a dispatch while the model was made again: %.1f ms (%s)\n", host.longestCallMs, host.longestCallMs < 50.0 ? "NO STALL" : "STALLED");

        // frame generation switched off: Lossless Scaling keeps presenting the scaled frame, but runs no LSFG pass, so the model has nothing to
        // run on. Its last result must not stay on the screen.
        // (With presentMode on, the addon takes the presented frames after a few presents without a capture and the picture changes again: the
        // matrix runs this with presentMode=0, and a longer stretch, offframes=, with it on.)
        uint64_t stale = 0;
        int offFrames = 25; for (int i = 4; i < argc; ++i) if (!strncmp(argv[i], "offframes=", 10)) offFrames = std::clamp(atoi(argv[i] + 10), 25, 600);
        for (int fr = 0; fr < offFrames; ++fr) {
            ID3D11Texture2D* bb = nullptr; sc->GetBuffer(0, IID_PPV_ARGS(&bb));
            if (fr >= offFrames - 5) { double m = 0; stale += CountChanged(dev, dc, bb, W2, H2, &m, nullptr); }   // what an earlier present of this stretch showed
            dc->CopyResource(bb, c2);
            bb->Release(); sc->Present(0, 0); pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        printf("[check-off] frame generation off for a second: %llu pixels changed on the last presents (%s)\n", (unsigned long long)stale, stale == 0 ? "OLD RESULT DROPPED" : "OLD RESULT STILL SHOWN");

        // hdr=: frame generation still off, the swap chain now HDR: a 16-bit float scRGB one, or a 10-bit one holding HDR10. The model takes the
        // presented frames (their SDR view) and its result goes back into them: the picture must change, the highlights far above SDR white
        // must stay as they were, and no NaN may be written.
        if (hdrMode) {
            const bool pq = hdrMode == 2;
            const DXGI_FORMAT hf = pq ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
            ID3D11Texture2D* pic = MakeHdrPicture(dev, dc, W2, H2, pq);
            const HRESULT hr = sc->ResizeBuffers(2, W2, H2, hf, 0);
            if (!pic || FAILED(hr)) printf("[check-hdr] the HDR swap chain could not be made (0x%08x)\n", (unsigned)hr);
            else {
                HdrCheck last; int samples = 0, applied = 0;
                for (int fr = 0; fr < 300; ++fr) {
                    ID3D11Texture2D* bb = nullptr; sc->GetBuffer(0, IID_PPV_ARGS(&bb));
                    if (fr >= 200 && fr % 10 == 0) {   // what the present two back showed, the addon's work on it included
                        const HdrCheck c = CheckHdr(dev, dc, bb, W2, H2, pq); samples++;
                        if (c.baseChanged > c.base / 20) { applied++; last = c; } else if (!last.base) last = c;
                    }
                    dc->CopyResource(bb, pic);
                    bb->Release(); sc->Present(0, 0); pump();
                    if (fr % 60 == 0) frame("hdr"); else emptyFrame();
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
                printf("[check-hdr] %s: the picture changed on %llu of %llu pixels (mean %.4f) in %d of %d samples (%s); highlights changed on %llu of %llu (%s); "
                       "NaN or infinity: %llu (%s)\n", pq ? "HDR10 (PQ)" : "scRGB", (unsigned long long)last.baseChanged, (unsigned long long)last.base,
                       last.baseChanged ? last.baseChange / last.baseChanged : 0.0, applied, samples, applied >= samples / 2 && samples ? "HDR COMPOSED" : "HDR NOT COMPOSED",
                       (unsigned long long)last.bandChanged, (unsigned long long)last.band, last.bandChanged <= last.band / 100 ? "HIGHLIGHTS KEPT" : "HIGHLIGHTS CHANGED",
                       (unsigned long long)last.bad, last.bad == 0 ? "CLEAN" : "BAD VALUES");
            }
            if (pic) pic->Release();
        }

        // replay=: a recording (.lsrec, as the addon's recorder saves them) presented frame by frame at its own pace, with frame generation
        // off, so the model takes the frames at Present as it would in the game. Looped until at least 300 presents. Every tenth present
        // (after the first 120) the buffer the addon composed two presents ago is compared with the frame that went into it: the model's
        // result must be on it. replayOut= writes it as a picture every 30 presents.
        if (!replayPath.empty()) {
            nr::lsrec::Reader rec; std::string error;
            std::wstring widePath(replayPath.size(), L'\0');
            widePath.resize(std::max(0, MultiByteToWideChar(CP_UTF8, 0, replayPath.c_str(), (int)replayPath.size(), widePath.data(), (int)widePath.size())));
            if (!rec.Open(widePath, &error) || !rec.Count()) printf("[check-replay] %s: %s\n", replayPath.c_str(), error.empty() ? "no frames" : error.c_str());
            else {
                const nr::lsrec::FileHeader& rh = rec.Header();
                const DXGI_FORMAT rf = (DXGI_FORMAT)rh.format;
                D3D11_TEXTURE2D_DESC td{}; td.Width = rh.width; td.Height = rh.height; td.MipLevels = 1; td.ArraySize = 1; td.Format = rf; td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                ID3D11Texture2D* src = nullptr; dev->CreateTexture2D(&td, nullptr, &src);
                const HRESULT hr = sc->ResizeBuffers(2, rh.width, rh.height, rf, 0);
                if (!src || FAILED(hr)) printf("[check-replay] a %ux%u swap chain of format %u could not be made (0x%08x)\n", rh.width, rh.height, rh.format, (unsigned)hr);
                else {
                    if (!replayOut.empty()) CreateDirectoryA(replayOut.c_str(), nullptr);
                    const size_t total = std::max<size_t>(300, rec.Count());
                    std::vector<uint8_t> px, history[2];
                    int samples = 0, composed = 0, pictures = 0; bool readFailed = false;
                    for (size_t k = 0; k < total && !readFailed; ++k) {
                        const size_t i = k % rec.Count();
                        if (!rec.Read(i, px)) { readFailed = true; break; }
                        ID3D11Texture2D* bb = nullptr; sc->GetBuffer(0, IID_PPV_ARGS(&bb));
                        const bool sample = k >= 120 && k % 10 == 0 && rh.bytesPerPixel == 4 && !history[k % 2].empty();
                        const bool picture = !replayOut.empty() && k >= 120 && k % 30 == 0 && rh.bytesPerPixel == 4;
                        if (sample || picture) {
                            D3D11_TEXTURE2D_DESC sd = td; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                            ID3D11Texture2D* st = nullptr; dev->CreateTexture2D(&sd, nullptr, &st);
                            D3D11_MAPPED_SUBRESOURCE m{};
                            if (st) { dc->CopyResource(st, bb); }
                            if (st && SUCCEEDED(dc->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                                const std::vector<uint8_t>& before = history[k % 2];
                                uint64_t changed = 0;
                                for (UINT y = 0; y < rh.height; ++y) {
                                    const uint8_t* row = (const uint8_t*)m.pData + y * m.RowPitch;
                                    if (sample) for (UINT x = 0; x < rh.width; ++x) if (memcmp(row + x * 4, before.data() + ((size_t)y * rh.width + x) * 4, 3) != 0) ++changed;
                                }
                                if (sample) { samples++; if (changed > (uint64_t)rh.width * rh.height / 20) composed++; }
                                if (picture) {
                                    char name[64]; snprintf(name, sizeof name, "/replay_%05zu.bmp", k);
                                    FILE* fp = fopen((replayOut + name).c_str(), "wb");
                                    if (fp) {
                                        const uint32_t rowBytes = rh.width * 4, img = rowBytes * rh.height, fsz = 54 + img, off = 54, ihs = 40; uint8_t fh[14] = { 'B', 'M' }; memcpy(fh + 2, &fsz, 4); memcpy(fh + 10, &off, 4);
                                        uint8_t ih[40] = {}; memcpy(ih, &ihs, 4); int32_t wv = (int32_t)rh.width, hv = -(int32_t)rh.height; memcpy(ih + 4, &wv, 4); memcpy(ih + 8, &hv, 4);
                                        uint16_t planes = 1, bpp = 32; memcpy(ih + 12, &planes, 2); memcpy(ih + 14, &bpp, 2); memcpy(ih + 20, &img, 4);
                                        fwrite(fh, 1, 14, fp); fwrite(ih, 1, 40, fp);
                                        std::vector<uint8_t> line(rowBytes);
                                        for (UINT y = 0; y < rh.height; ++y) {
                                            memcpy(line.data(), (const uint8_t*)m.pData + y * m.RowPitch, rowBytes);
                                            if (rf == DXGI_FORMAT_R8G8B8A8_UNORM) for (UINT x = 0; x < rh.width; ++x) std::swap(line[x * 4], line[x * 4 + 2]);
                                            fwrite(line.data(), 1, rowBytes, fp);
                                        }
                                        fclose(fp); pictures++;
                                    }
                                }
                                dc->Unmap(st, 0);
                            }
                            if (st) st->Release();
                        }
                        dc->UpdateSubresource(src, 0, nullptr, px.data(), rh.width * rh.bytesPerPixel, 0);
                        dc->CopyResource(bb, src);
                        bb->Release();
                        history[k % 2] = px;
                        sc->Present(0, 0); pump();
                        if (k % 60 == 0) frame("replay"); else emptyFrame();
                        // the recording's own pace (4 to 50 ms a frame)
                        const size_t next = (k + 1) % rec.Count();
                        double ms = 16.7;
                        if (next && rh.qpcFrequency) ms = (double)(rec.FrameInfo(next).qpc - rec.FrameInfo(i).qpc) * 1000.0 / rh.qpcFrequency;
                        std::this_thread::sleep_for(std::chrono::microseconds((int64_t)(std::clamp(ms, 4.0, 50.0) * 1000.0)));
                    }
                    printf("[check-replay] %zu frames of %ux%u (format %u) from '%s', %zu presents: the model's result was on %d of %d sampled presents (%s); %d pictures%s\n",
                           rec.Count(), rh.width, rh.height, rh.format, rh.game, total, composed, samples,
                           readFailed ? "A FRAME COULD NOT BE READ" : samples && composed >= samples / 2 ? "REPLAY COMPOSED" : "REPLAY NOT COMPOSED", pictures,
                           replayOut.empty() ? "" : (" in " + replayOut).c_str());
                }
                if (src) src->Release();
            }
        }

    }
    host.PublishEvent(EAM_EVENT_D3D11_DEVICE_CHANGED, nullptr, 0);
    for (int i = 4; i < argc; ++i) if (!strcmp(argv[i], "exitmode=abrupt")) {
        // What Lossless Scaling does at exit: the process ends with the addon still loaded and AddonShutdown never called. The addon's static
        // destructors then run in DLL_PROCESS_DETACH, and a std::thread still joinable there calls std::terminate.
        printf("done (abrupt exit: no AddonShutdown)\n"); fflush(stdout);
        ExitProcess(0);
    }
    Shut();
    if (Shut2) Shut2();
    for (int i = 4; i < argc; ++i) if (!strcmp(argv[i], "unload=1")) {
        // What the manager does when the addon is switched off while Lossless Scaling runs: AddonShutdown, then FreeLibrary. Then Lossless
        // Scaling makes a new D3D11 device (it does when scaling restarts). With DLSS's NGX library once started, that hung Lossless Scaling
        // (2026-09-24) until the addon's DLL was kept loaded. A watchdog reports a hang instead of waiting for ever.
        FreeLibrary(h);
        char loadedName[MAX_PATH] = {};
        const bool stillLoaded = GetModuleFileNameA(h, loadedName, MAX_PATH) != 0;   // still mapped after FreeLibrary: pinned
        std::atomic<bool> made{ false };
        const auto t0 = std::chrono::steady_clock::now();
        std::thread([&made] {
            ID3D11Device* d = nullptr; ID3D11DeviceContext* c = nullptr;
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, nullptr, &c);
            if (c) c->Release();
            if (d) d->Release();
            made = true;
        }).detach();
        while (!made && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("[check-unload] addon DLL %s after FreeLibrary; a new D3D11 device %s (%.0f ms)\n", stillLoaded ? "still loaded (pinned)" : "unloaded",
               made ? "was made" : "HUNG", ms);
        if (!made) { fflush(stdout); ExitProcess(3); }
    }
    if (shotMode) shot.Shutdown();
    sc->Release(); DestroyWindow(hwnd);
    ImGui::DestroyContext(ctx);
    printf("done\n");
    return 0;
}
