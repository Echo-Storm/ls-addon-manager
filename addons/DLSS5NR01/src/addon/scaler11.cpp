#include "addon/scaler11.h"
#include "addon/bridge.h"
#include "addon/product.h"
#include "engine/sr_engine.h"
#include "engine/hdr_hlsl.h"
#include <string>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace nr {

namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool Texture2D(ID3D11Resource* r, D3D11_TEXTURE2D_DESC& desc) {
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&desc);
    return true;
}

// The pass's own bindings (and its shader: the grab pass sets ours) are taken off while our work runs (its output is bound as a UAV, which a
// copy must not write under) and put back after, so the NIS dispatch and the passes that follow find what they left.
struct SavedBindings {
    static const UINT kSrvs = 8, kUavs = 4;
    ID3D11DeviceContext* ctx;
    ID3D11ShaderResourceView* srvs[kSrvs] = {}; ID3D11UnorderedAccessView* uavs[kUavs] = {};
    ID3D11ComputeShader* shader = nullptr;
    explicit SavedBindings(ID3D11DeviceContext* c) : ctx(c) {
        ctx->CSGetShaderResources(0, kSrvs, srvs); ctx->CSGetUnorderedAccessViews(0, kUavs, uavs); ctx->CSGetShader(&shader, nullptr, nullptr);
        ID3D11ShaderResourceView* noSrvs[kSrvs] = {}; ID3D11UnorderedAccessView* noUavs[kUavs] = {};
        ctx->CSSetShaderResources(0, kSrvs, noSrvs); ctx->CSSetUnorderedAccessViews(0, kUavs, noUavs, nullptr);
    }
    ~SavedBindings() {
        ctx->CSSetShader(shader, nullptr, 0);
        ctx->CSSetShaderResources(0, kSrvs, srvs); ctx->CSSetUnorderedAccessViews(0, kUavs, uavs, nullptr);
        for (auto*& v : srvs) SafeRelease(v);
        for (auto*& v : uavs) SafeRelease(v);
        SafeRelease(shader);
    }
};

// The grab pass: the frame, read the way NIS reads it (through the pass's t0 view), written into the shared frame texture; from the input
// viewport's corner on (0, 0 for the whole frame). With the picture controls set, they are applied on the way, as Neural Rendering's
// Picture controls do them (compose11.cpp: Tone, then TonalRanges, then Colour): the upscaler takes the game as it then looks, at the small
// size, so they cost next to nothing.
const char* const kGrabHlsl = R"HLSL(
Texture2D<float4>   tFrame : register(t0);
RWTexture2D<float4> uOut   : register(u0);
cbuffer C : register(b0) {
    uint2 origin; uint tone; float brightness;
    float contrast; float gamma; float shadows; float highlights;
    float saturation; float vibrance; uint encoding; float white;   // encoding: 0 SDR, 1 scRGB, 2 HDR10 (hdr_hlsl.h)
};
static const float3 kLuma = float3(0.299, 0.587, 0.114);
[numthreads(8, 8, 1)]
void CSGrab(uint3 id : SV_DispatchThreadID) {
    uint w, h; uOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float4 c = tFrame.Load(int3(id.xy + origin, 0));
    c.rgb = ToSdr(c.rgb, encoding, white);   // an HDR frame's SDR view (SDR: as it is)
    if (tone != 0u) {
        c.rgb = saturate((c.rgb - 0.5) * contrast + 0.5 + brightness);                  // tone, as a monitor's controls
        c.rgb = pow(max(c.rgb, 1e-5), 1.0 / max(gamma, 0.05));
        float l = dot(c.rgb, kLuma);                                                     // shadows fade out by 0.55 luma, highlights in from 0.45
        c.rgb = saturate(c.rgb + (shadows * (1.0 - smoothstep(0.0, 0.55, l)) + highlights * smoothstep(0.45, 1.0, l)) * 0.25);
        l = dot(c.rgb, kLuma);                                                           // saturation; vibrance only where colour is muted
        const float spread = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
        c.rgb = saturate(l + (c.rgb - l) * (saturation + vibrance * (1.0 - saturate(spread))));
    }
    uOut[id.xy] = c;
}
)HLSL";

// The place pass (HDR frames only): the upscaler's picture, made in the frame's SDR view, back into the frame's own encoding, written into
// NIS's output (at the output viewport's corner). SDR frames are copied as they are, without it.
const char* const kPlaceHlsl = R"HLSL(
Texture2D<float4>   tPicture : register(t0);
RWTexture2D<float4> uOut     : register(u0);
cbuffer C : register(b0) { uint2 origin; uint2 size; uint encoding; float white; float2 unused; };
[numthreads(8, 8, 1)]
void CSPlace(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) return;
    const float4 p = tPicture.Load(int3(id.xy, 0));
    uOut[id.xy + origin] = float4(FromSdr(p.rgb, encoding, white), p.a);
}
)HLSL";

// ---- NIS's viewports
//
// NVIDIA's NIS scales between two viewports, which its constant buffer (NISConfig, b0) holds, and dispatches one group per 32x24 pixels of
// the output viewport. For a window of the screen's shape both are the whole textures; for another shape Lossless Scaling scales the window
// into part of the screen. The constants are read once per pass shape without waiting (copied now, mapped on a later pass), and only a
// layout that agrees with everything else (the dispatch, the textures, NIS's own scale factor) is used.
struct NisConfigView {   // NISConfig (NVIDIA Image Scaling SDK 1.0), the part read here: 18 floats, then the viewports
    float f[18]; uint32_t inX, inY, inW, inH, outX, outY, outW, outH;
};
static_assert(sizeof(NisConfigView) == 104, "NISConfig's viewports start at byte 72");

struct ViewportReader {
    struct Key { uint32_t inW, inH, outW, outH, x, y; bool operator==(const Key& k) const { return !memcmp(this, &k, sizeof k); } };
    ID3D11Device* dev = nullptr;
    ID3D11Buffer* staging = nullptr;
    Key key{}; int state = 0;         // 0 nothing, 1 a copy in flight, 2 known good, 3 known unusable
    NisConfigView cfg{};
    uint32_t tries = 0;
    void Reset() { if (staging) staging->Release(); staging = nullptr; dev = nullptr; state = 0; tries = 0; }
};
ViewportReader g_viewports;

// True with the viewports filled in, once known; false meanwhile (NIS runs as usual) and for a layout that does not fit.
bool ResolveViewports(ID3D11DeviceContext* ctx, const D3D11_TEXTURE2D_DESC& in, const D3D11_TEXTURE2D_DESC& o, uint32_t x, uint32_t y, NisPass& pass,
                      const std::function<void(const char*)>& log) {
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    if (dev) dev->Release();   // only compared
    const ViewportReader::Key key{ in.Width, in.Height, o.Width, o.Height, x, y };
    ViewportReader& r = g_viewports;
    if (r.dev != dev || !(r.key == key)) { r.Reset(); r.dev = dev; r.key = key; }
    auto say = [&](const char* fmt, auto... args) { if (log) { char text[400]; snprintf(text, sizeof text, fmt, args...); log(text); } };
    if (r.state == 0) {
        if (++r.tries > 3) { r.state = 3; return false; }
        ID3D11Buffer* cb = nullptr; ctx->CSGetConstantBuffers(0, 1, &cb);
        if (!cb) { say("NIS pass on part of its output (%ux%u -> %ux%u, %ux%u groups): it has no constants bound; NIS stays", in.Width, in.Height, o.Width, o.Height, x, y); r.state = 3; return false; }
        D3D11_BUFFER_DESC d{}; cb->GetDesc(&d);
        if (d.ByteWidth < sizeof(NisConfigView)) { say("NIS pass on part of its output: its constants are %u bytes, too few for NIS's; NIS stays", d.ByteWidth); cb->Release(); r.state = 3; return false; }
        D3D11_BUFFER_DESC sd{}; sd.ByteWidth = d.ByteWidth; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (!r.staging && FAILED(dev->CreateBuffer(&sd, nullptr, &r.staging))) { cb->Release(); r.state = 3; return false; }
        ctx->CopyResource(r.staging, cb);
        cb->Release();
        r.state = 1;
        return false;
    }
    if (r.state == 1) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (ctx->Map(r.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) return false;   // not back yet
        memcpy(&r.cfg, m.pData, sizeof r.cfg);
        ctx->Unmap(r.staging, 0);
        const NisConfigView& c = r.cfg;
        const float scaleX = c.outW ? static_cast<float>(c.inW) / c.outW : 0.0f, scaleY = c.outH ? static_cast<float>(c.inH) / c.outH : 0.0f;
        const bool fits = c.inW && c.inH && c.outW && c.outH && c.inX + c.inW <= in.Width && c.inY + c.inH <= in.Height &&
                          c.outX + c.outW <= o.Width && c.outY + c.outH <= o.Height && x == (c.outW + 31) / 32 && y == (c.outH + 23) / 24 &&
                          c.outW >= c.inW && c.outH >= c.inH &&
                          std::abs(c.f[12] - scaleX) < 0.02f * scaleX + 1e-4f && std::abs(c.f[13] - scaleY) < 0.02f * scaleY + 1e-4f;   // kScaleX, kScaleY
        say("NIS pass on part of its output: frame %ux%u, output %ux%u, %ux%u groups; its constants: input viewport %u,%u %ux%u, output viewport %u,%u %ux%u, "
            "scale %.4f x %.4f -> %s", in.Width, in.Height, o.Width, o.Height, x, y, c.inX, c.inY, c.inW, c.inH, c.outX, c.outY, c.outW, c.outH, c.f[12], c.f[13],
            fits ? "the upscaler takes that part" : "they do not fit together; NIS stays");
        r.state = fits ? 2 : 3;
        if (!fits) return false;
    }
    if (r.state != 2) return false;
    const NisConfigView& c = r.cfg;
    pass.inX = c.inX; pass.inY = c.inY; pass.inW = c.inW; pass.inH = c.inH;
    pass.outX = c.outX; pass.outY = c.outY; pass.outW = c.outW; pass.outH = c.outH;
    return true;
}

} // namespace

// ---- recognising the NIS pass

namespace { bool g_lastRefused = false; }   // the last FindNisPass was a NIS pass whose layout could not be followed (render thread only)

bool FindNisPass(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z, NisPass& pass, const std::function<void(const char*)>& log) {
    pass = {};
    g_lastRefused = false;
    if (z != 1) return false;
    ID3D11ShaderResourceView* srvs[3] = {}; ID3D11UnorderedAccessView* uav = nullptr;
    ctx->CSGetShaderResources(0, 3, srvs); ctx->CSGetUnorderedAccessViews(0, 1, &uav);
    ID3D11Resource* res[3] = {}; ID3D11Resource* out = nullptr;
    for (int i = 0; i < 3; ++i) if (srvs[i]) srvs[i]->GetResource(&res[i]);
    if (uav) uav->GetResource(&out);
    D3D11_TEXTURE2D_DESC in{}, c1{}, c2{}, o{};
    auto coefficients = [](const D3D11_TEXTURE2D_DESC& d) { return d.Width == 2 && d.Height == 64 && d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT; };
    const bool nis = Texture2D(res[0], in) && Texture2D(res[1], c1) && Texture2D(res[2], c2) && Texture2D(out, o) && coefficients(c1) && coefficients(c2);
    const bool whole = nis && o.Width >= in.Width && o.Height >= in.Height &&   // 1:1 too (DLSS then runs as DLAA)
                       x == (o.Width + 31) / 32 && y == (o.Height + 23) / 24;
    for (auto*& v : srvs) SafeRelease(v);
    SafeRelease(uav); SafeRelease(res[1]); SafeRelease(res[2]);
    // NIS's bindings with a dispatch over less than the output: a window of another shape, scaled into part of the screen
    const bool part = nis && !whole && x <= (o.Width + 31) / 32 && y <= (o.Height + 23) / 24 && ResolveViewports(ctx, in, o, x, y, pass, log);
    g_lastRefused = nis && !whole && !part && g_viewports.state == 3;   // this very pass (not a verdict kept from another window shape)
    if (!whole && !part) { SafeRelease(res[0]); SafeRelease(out); pass = {}; return false; }
    pass.in = res[0]; pass.out = out; pass.inFmt = in.Format; pass.outFmt = o.Format;
    if (whole) { pass.inW = in.Width; pass.inH = in.Height; pass.outW = o.Width; pass.outH = o.Height; }
    return true;
}

void ReleaseNisPass(NisPass& pass) { SafeRelease(pass.in); SafeRelease(pass.out); pass = {}; }

bool NisLayoutRefused() { return g_lastRefused; }

// ---- the link to the engine

void ScalerLink::Shared::Release() { SafeRelease(d3d11); SafeRelease(d3d12); w = h = 0; fmt = DXGI_FORMAT_UNKNOWN; }
void ScalerLink::Fence::Release() { SafeRelease(d3d11); SafeRelease(d3d12); }

void ScalerLink::Log(const char* fmt, ...) {
    if (!m_log) return;
    char text[512]; va_list a; va_start(a, fmt); vsnprintf(text, sizeof text, fmt, a); va_end(a);
    m_log(text);
}

bool ScalerLink::MakeFence(Fence& f, const char* name) {
    if (FAILED(m_dev->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f.d3d11)))) { Log("%s upscaler: the %s fence could not be made", kUpscalerName, name); return false; }
    HANDLE handle = nullptr;
    if (FAILED(f.d3d11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle))) { Log("%s upscaler: the %s fence could not be shared", kUpscalerName, name); f.Release(); return false; }
    f.d3d12 = m_engine->OpenSharedFence(handle);
    CloseHandle(handle);
    if (!f.d3d12) { f.Release(); return false; }
    return true;
}

// A texture on Lossless Scaling's device, opened on the engine's. The engine only reads the frame and flow copies; it writes the picture.
bool ScalerLink::Fit(Shared& t, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool engineWrites, const char* name) {
    if (t.d3d11 && t.w == w && t.h == h && t.fmt == fmt) return true;
    if (t.d3d11) { m_engine->Drain(); t.Release(); }   // the engine may still read or write the old one
    D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (engineWrites ? D3D11_BIND_UNORDERED_ACCESS : 0u);
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = m_dev->CreateTexture2D(&d, nullptr, &t.d3d11);
    if (SUCCEEDED(hr)) {
        IDXGIResource1* dxgi = nullptr; HANDLE handle = nullptr;
        hr = t.d3d11->QueryInterface(IID_PPV_ARGS(&dxgi));
        if (SUCCEEDED(hr)) { hr = dxgi->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle); dxgi->Release(); }
        if (SUCCEEDED(hr)) { t.d3d12 = m_engine->OpenSharedTexture(handle); CloseHandle(handle); if (!t.d3d12) hr = E_FAIL; }
    }
    if (FAILED(hr)) { Log("%s upscaler: the shared %s (%ux%u, format %d) could not be made: 0x%08x", kUpscalerName, name, w, h, (int)fmt, (unsigned)hr); t.Release(); return false; }
    t.w = w; t.h = h; t.fmt = fmt;
    Log("%s upscaler: shared %s %ux%u, format %d", kUpscalerName, name, w, h, (int)fmt);
    return true;
}

bool ScalerLink::Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, SrEngine* engine, LogFn log) {
    m_log = std::move(log); m_engine = engine; m_ctx = ctx; m_frame = 0; for (auto& h : m_holds) h = 0; m_described = false; m_loggedPartial = false;
    m_count = Counters(); m_lastShown = 0;
    m_atPresent = m_copiedAtPresent = 0; m_probeState = 0; m_pendingReset = false;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&m_dev))) || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&m_ctx4)))) {
        Log("%s upscaler: Lossless Scaling's device has no shared fences (D3D11.4 is needed)", kUpscalerName); Shutdown(); return false;
    }
    if (!MakeFence(m_copied, "copied") || !MakeFence(m_done, "done") || !MakeGrabShader() || !MakePlaceShader()) { Shutdown(); return false; }
    return true;
}

bool ScalerLink::MakeGrabShader() {
    ID3DBlob* code = nullptr, * error = nullptr;
    const std::string source = std::string(NR_HDR_HLSL) + kGrabHlsl;
    if (FAILED(D3DCompile(source.c_str(), source.size(), "scaler_grab", nullptr, nullptr, "CSGrab", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error))) {
        Log("%s upscaler: the grab shader: %s", kUpscalerName, error ? static_cast<const char*>(error->GetBufferPointer()) : "?"); SafeRelease(error); return false;
    }
    const HRESULT hr = m_dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m_grab);
    code->Release();
    if (FAILED(hr)) { Log("%s upscaler: the grab shader could not be made: 0x%08x", kUpscalerName, (unsigned)hr); return false; }
    D3D11_BUFFER_DESC cb{}; cb.ByteWidth = 48; cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_dev->CreateBuffer(&cb, nullptr, &m_grabOrigin))) { Log("%s upscaler: the grab's constants could not be made", kUpscalerName); return false; }
    return true;
}

bool ScalerLink::MakePlaceShader() {
    ID3DBlob* code = nullptr, * error = nullptr;
    const std::string source = std::string(NR_HDR_HLSL) + kPlaceHlsl;
    if (FAILED(D3DCompile(source.c_str(), source.size(), "scaler_place", nullptr, nullptr, "CSPlace", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error))) {
        Log("%s upscaler: the place shader: %s", kUpscalerName, error ? static_cast<const char*>(error->GetBufferPointer()) : "?"); SafeRelease(error); return false;
    }
    const HRESULT hr = m_dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m_place);
    code->Release();
    if (FAILED(hr)) { Log("%s upscaler: the place shader could not be made: 0x%08x", kUpscalerName, (unsigned)hr); return false; }
    D3D11_BUFFER_DESC cb{}; cb.ByteWidth = 32; cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_dev->CreateBuffer(&cb, nullptr, &m_placeConstants))) { Log("%s upscaler: the place pass's constants could not be made", kUpscalerName); return false; }
    return true;
}

void ScalerLink::Unblock() {
    if (m_copied.d3d12 && m_copied.d3d12->GetCompletedValue() < m_frame) {
        Log("%s upscaler: the frame-copied signal had reached %llu of %llu; signalled from the CPU so the engine can finish", kUpscalerName,
            (unsigned long long)m_copied.d3d12->GetCompletedValue(), (unsigned long long)m_frame);
        m_copied.d3d12->Signal(m_frame);
    }
}

void ScalerLink::Shutdown() {
    m_grabbed = nullptr;
    Unblock();
    if (m_engine && (m_in[0].d3d12 || m_copied.d3d12)) m_engine->Drain();
    // Lossless Scaling's queue may be waiting on the GPU for a picture (gpuWait): should the engine not have finished it, release the wait
    if (m_done.d3d12 && m_done.d3d12->GetCompletedValue() < m_frame) {
        Log("%s upscaler: the engine had finished %llu of %llu frames; released Lossless Scaling's wait for them", kUpscalerName,
            (unsigned long long)m_done.d3d12->GetCompletedValue(), (unsigned long long)m_frame);
        m_done.d3d12->Signal(m_frame);
    }
    for (auto*& v : m_inUav) SafeRelease(v);
    SafeRelease(m_grab); SafeRelease(m_grabOrigin);
    SafeRelease(m_place); SafeRelease(m_placeConstants); SafeRelease(m_placeUav); m_placeTarget = nullptr;
    for (int i = 0; i < kOut; ++i) { SafeRelease(m_outSrv[i]); m_outSrvFor[i] = nullptr; }
    m_loggedEncoding = ~0u;
    g_viewports.Reset();   // read again on the next device
    for (auto& t : m_in) t.Release();
    for (auto& t : m_out) t.Release();
    for (auto& t : m_flow) t.Release();
    for (auto& h : m_holds) h = 0;
    m_atPresent = 0;
    SafeRelease(m_probe[0]); SafeRelease(m_probe[1]);
    m_copied.Release(); m_done.Release();
    SafeRelease(m_ctx4); SafeRelease(m_dev); m_ctx = nullptr;
}

void ScalerLink::ReportDeviceChange() {
    if (!m_dev) return;
    const HRESULT reason = m_dev->GetDeviceRemovedReason();
    const char* name = reason == S_OK ? "not removed: Lossless Scaling replaced it itself"
                     : reason == DXGI_ERROR_DEVICE_HUNG ? "HUNG (its GPU work stopped making progress)"
                     : reason == DXGI_ERROR_DEVICE_RESET ? "RESET" : reason == DXGI_ERROR_DEVICE_REMOVED ? "REMOVED" : "other";
    const uint64_t copied = m_copied.d3d11 ? m_copied.d3d11->GetCompletedValue() : 0, done = m_done.d3d11 ? m_done.d3d11->GetCompletedValue() : 0;
    Log("%s upscaler: Lossless Scaling's device changed; the old one: 0x%08x %s. Frames handed over %llu, 'copied' reached %llu, 'done' reached %llu", kUpscalerName,
        (unsigned)reason, name, (unsigned long long)m_frame, (unsigned long long)copied, (unsigned long long)done);
}

// Once per link: what the pass reads and writes, so the log shows whether its output is the swap chain's own buffer.
void ScalerLink::DescribeTargets(const NisPass& pass) {
    if (m_described) return;
    m_described = true;
    auto describe = [&](const char* name, ID3D11Resource* r) {
        D3D11_TEXTURE2D_DESC d{}; if (!Texture2D(r, d)) return;
        DXGI_USAGE usage = 0; IDXGIResource* dxgi = nullptr;
        if (SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(&dxgi)))) { dxgi->GetUsage(&usage); dxgi->Release(); }
        Log("%s upscaler: the NIS pass's %s: %ux%u format %d, bind 0x%x, misc 0x%x, usage 0x%x%s", kUpscalerName, name, d.Width, d.Height, (int)d.Format,
            d.BindFlags, d.MiscFlags, (unsigned)usage, (usage & DXGI_USAGE_BACK_BUFFER) ? " (the swap chain's back buffer)" : "");
    };
    describe("input", pass.in);
    describe("output", pass.out);
    ID3D11ShaderResourceView* view = nullptr; m_ctx->CSGetShaderResources(0, 1, &view);   // how NIS reads the frame (an sRGB view would linearise it)
    if (view) { D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; view->GetDesc(&vd); Log("%s upscaler: the NIS pass reads the frame as format %d", kUpscalerName, (int)vd.Format); view->Release(); }
}

bool ScalerLink::Upscale(const NisPass& pass, ID3D11Resource* flow, uint32_t flowW, uint32_t flowH, float flowUnit, float motionFraction, bool estimate, unsigned preset,
                         float sharpen, bool reset, Handoff handoff, bool gpuWait) {
    if (!IsReady() || !m_engine || !m_engine->IsReady()) return false;
    DescribeTargets(pass);
    if (handoff != m_handoff) {
        Log("%s upscaler: handoff %s", kUpscalerName, handoff == Handoff::Late ? "the newest finished picture (nothing waits)" : handoff == Handoff::Wait ? "GPU wait"
                                         : handoff == Handoff::Observe ? "observe only (NIS stays)" : "NIS runs, the picture copied over it at Present");
        Unblock(); m_engine->Drain();   // the variants keep their frames differently: start from an idle engine
        m_handoff = handoff; for (auto& h : m_holds) h = 0;
    }
    // the shared textures: the frame as RGBA8 (the grab pass reads BGRA as RGBA), the picture in the output's format, which the upscaler
    // writes through a UAV
    // 10-bit and half-float frames (HDR, or 10-bit SDR) go through a half-float frame texture, so their SDR view keeps its precision
    const DXGI_FORMAT outFmt = Bridge::ViewFormat(pass.outFmt), inView = Bridge::ViewFormat(pass.inFmt);
    const bool in8 = inView == DXGI_FORMAT_R8G8B8A8_UNORM || inView == DXGI_FORMAT_B8G8R8A8_UNORM;
    const DXGI_FORMAT inFmt = in8 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
    m_refusedFormat = (!in8 && inView != DXGI_FORMAT_R10G10B10A2_UNORM && inView != DXGI_FORMAT_R16G16B16A16_FLOAT) ||
                      (outFmt != DXGI_FORMAT_R8G8B8A8_UNORM && outFmt != DXGI_FORMAT_R10G10B10A2_UNORM && outFmt != DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (m_refusedFormat) {
        if (!m_loggedFormat) { Log("%s upscaler: frame format %d -> %d is not one the upscaler can take here; NIS stays", kUpscalerName, (int)pass.inFmt, (int)pass.outFmt); m_loggedFormat = true; }
        return false;
    }
    static const char* const inNames[kIn] = { "frame", "second frame" };
    static const char* const outNames[kOut] = { "picture", "second picture", "third picture" };
    bool refit = true;
    for (int i = 0; i < kIn && refit; ++i) {
        ID3D11Texture2D* const before = m_in[i].d3d11;
        refit = Fit(m_in[i], pass.inW, pass.inH, inFmt, true, inNames[i]);
        if (refit && (m_in[i].d3d11 != before || !m_inUav[i])) {
            SafeRelease(m_inUav[i]);
            if (FAILED(m_dev->CreateUnorderedAccessView(m_in[i].d3d11, nullptr, &m_inUav[i]))) { Log("%s upscaler: the frame's UAV could not be made", kUpscalerName); return false; }
        }
    }
    for (int i = 0; i < kOut && refit; ++i) {
        ID3D11Texture2D* const before = m_out[i].d3d11;
        refit = Fit(m_out[i], pass.outW, pass.outH, outFmt, true, outNames[i]);
        if (refit && m_out[i].d3d11 != before) m_holds[i] = 0;   // a new texture holds nothing yet
    }
    if (!refit) return false;
    ++m_count.passes;
    LARGE_INTEGER qpcFreq, qpcNow; QueryPerformanceFrequency(&qpcFreq); QueryPerformanceCounter(&qpcNow);
    const bool close = m_lastPassQpc && (qpcNow.QuadPart - m_lastPassQpc) * 1000.0 / qpcFreq.QuadPart < Counters::kClosePassMs;
    m_lastPassQpc = qpcNow.QuadPart;
    if (close) ++m_count.closePasses;

    // A new frame goes to the engine while fewer than kIn are with it (its queue finishes them in order, so "done" says how many are left).
    // With Wait, Lossless Scaling's own queue waited for the one before, so there is room.
    const uint64_t finished = m_done.d3d11->GetCompletedValue();
    const bool room = m_handoff == Handoff::Wait || m_frame < finished + kIn;
    m_pendingReset = m_pendingReset || reset;   // not lost when a frame is not handed over
    const SavedBindings saved(m_ctx);
    if (room) {
        const uint64_t n = ++m_frame;
        const int in = static_cast<int>(n % kIn), out = static_cast<int>(n % kOut);
        ID3D11Texture2D* flowTex = nullptr;
        if (!estimate && flow && flowW && flowH && Fit(m_flow[in], flowW, flowH, DXGI_FORMAT_R16G16B16A16_FLOAT, false, in ? "second flow" : "flow")) flowTex = m_flow[in].d3d11;
        ID3D11ShaderResourceView* frame = saved.srvs[0];   // the NIS pass's own view of the frame
        ID3D11Buffer* nisConstants = nullptr; m_ctx->CSGetConstantBuffers(0, 1, &nisConstants);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(m_ctx->Map(m_grabOrigin, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            struct { uint32_t x, y, tone; float brightness, contrast, gamma, shadows, highlights, saturation, vibrance; uint32_t encoding; float white; } constants = {
                pass.inX, pass.inY, m_picture.Neutral() ? 0u : 1u, m_picture.brightness, m_picture.contrast, m_picture.gamma,
                m_picture.shadows, m_picture.highlights, m_picture.saturation, m_picture.vibrance, m_encoding, m_white };
            static_assert(sizeof constants == 48, "the grab's cbuffer");
            memcpy(mapped.pData, &constants, sizeof constants); m_ctx->Unmap(m_grabOrigin, 0);
        }
        m_ctx->CSSetShader(m_grab, nullptr, 0);
        m_ctx->CSSetShaderResources(0, 1, &frame);
        m_ctx->CSSetUnorderedAccessViews(0, 1, &m_inUav[in], nullptr);
        m_ctx->CSSetConstantBuffers(0, 1, &m_grabOrigin);
        m_ctx->Dispatch((pass.inW + 7) / 8, (pass.inH + 7) / 8, 1);
        m_grabbed = m_in[in].d3d11;
        ID3D11ShaderResourceView* noSrv = nullptr; ID3D11UnorderedAccessView* noUav = nullptr;
        m_ctx->CSSetShaderResources(0, 1, &noSrv); m_ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
        m_ctx->CSSetConstantBuffers(0, 1, &nisConstants);   // NIS's, for the passes after (with NIS kept, its own dispatch)
        SafeRelease(nisConstants);
        if (flowTex) m_ctx->CopyResource(flowTex, flow);
        m_ctx4->Signal(m_copied.d3d11, n);
        m_ctx->Flush();   // the engine's queue waits for this signal: hand it to the GPU now
        // The engine signals "done" = n on its own queue in every case (after the frames before it), also when it could not run this one.
        const bool ran = m_engine->Run(m_in[in].d3d12, pass.inW, pass.inH, inFmt, m_out[out].d3d12, pass.outW, pass.outH, outFmt,
                                       flowTex ? m_flow[in].d3d12 : nullptr, m_flow[in].w, m_flow[in].h, flowUnit, motionFraction, estimate, preset, sharpen,
                                       m_pendingReset, m_copied.d3d12, n, m_done.d3d12, n);
        m_holds[out] = ran ? n : 0;
        if (ran) m_pendingReset = false;
        if (m_handoff == Handoff::Wait) {
            if (!ran) return false;
            m_ctx4->Wait(m_done.d3d11, n);   // a GPU wait: Lossless Scaling's queue holds until the picture is written
            return PlacePicture(pass, m_out[out].d3d11);
        }
    } else {
        ++m_count.skipped;
    }
    // The newest picture the engine has finished (never one it is writing: those are the kIn at most after it, in other textures).
    const uint64_t newest = m_done.d3d11->GetCompletedValue();
    Probe(newest, room);
    if ((m_count.passes % 3000) == 0)
        Log("%s upscaler: %llu passes (%llu within %.0f ms of the one before), %llu frames not handed over (the engine had %d already), %llu pictures "
            "shown twice (%llu of them close), waited on the GPU for the next picture %llu times (%llu close)", kUpscalerName, (unsigned long long)m_count.passes,
            (unsigned long long)m_count.closePasses, Counters::kClosePassMs, (unsigned long long)m_count.skipped, kIn, (unsigned long long)m_count.repeats,
            (unsigned long long)m_count.closeRepeats, (unsigned long long)m_count.waits, (unsigned long long)m_count.closeWaits);
    if (m_handoff == Handoff::Observe) return false;
    if (m_handoff == Handoff::AtPresent) { m_atPresent = newest && m_holds[newest % kOut] == newest ? newest : 0; return false; }   // NIS runs; the picture goes over it at Present
    // When the engine has finished nothing newer than the picture shown last (two passes close together, as adaptive frame generation makes
    // them), this pass would show it again: a visible hitch. With gpuWait, Lossless Scaling's queue waits on the GPU for the next picture and
    // shows that (the engine always signals "done" for every frame, and Shutdown releases the wait should it ever not). The CPU never waits:
    // a CPU wait shifted Lossless Scaling's timing and made repeats more frequent on a busy GPU (2026-09-25).
    uint64_t show = newest;
    const uint64_t next = m_lastShown + 1;
    if (m_handoff == Handoff::Late && gpuWait && m_lastShown && newest <= m_lastShown && next <= m_frame && m_holds[next % kOut] == next) {
        m_ctx4->Wait(m_done.d3d11, next);
        show = next;
        ++m_count.waits; if (close) ++m_count.closeWaits;
    }
    if (!show || m_holds[show % kOut] != show) return false;   // nothing finished yet: NIS this once
    if (show == m_lastShown) { ++m_count.repeats; if (close) ++m_count.closeRepeats; }
    m_lastShown = show;
    return PlacePicture(pass, m_out[show % kOut].d3d11);
}

// The picture into the pass's output: the whole of it, or its output viewport (the rest, Lossless Scaling's borders, is left as it is).
bool ScalerLink::PlacePicture(const NisPass& pass, ID3D11Texture2D* picture) {
    if (m_encoding != m_loggedEncoding) {
        m_loggedEncoding = m_encoding;
        if (m_encoding) Log("%s upscaler: HDR frames (%s, SDR white %.0f nits): upscaled in their SDR view, the picture put back in their own encoding",
                            kUpscalerName, m_encoding == 1 ? "scRGB" : "HDR10", m_white);
    }
    if (m_encoding) return PlaceHdr(pass, picture);
    if (!pass.Partial()) { m_ctx->CopyResource(pass.out, picture); return true; }
    m_ctx->CopySubresourceRegion(pass.out, 0, pass.outX, pass.outY, 0, picture, 0, nullptr);
    if (!m_loggedPartial) {
        m_loggedPartial = true;
        Log("%s upscaler: the window is scaled into part of the screen: %ux%u from %u,%u of the frame -> %ux%u at %u,%u", kUpscalerName, pass.inW, pass.inH,
            pass.inX, pass.inY, pass.outW, pass.outH, pass.outX, pass.outY);
    }
    return true;
}

// An HDR frame's picture into NIS's output through the place pass (FromSdr), on the pass's context. The caller's SavedBindings puts the
// pass's own views and shader back; its constant buffer is put back here.
bool ScalerLink::PlaceHdr(const NisPass& pass, ID3D11Texture2D* picture) {
    int slot = -1;
    for (int i = 0; i < kOut; ++i) if (m_out[i].d3d11 == picture) slot = i;
    if (slot < 0 || !m_place || !m_placeConstants) return false;
    if (m_outSrvFor[slot] != picture) {
        SafeRelease(m_outSrv[slot]); m_outSrvFor[slot] = nullptr;
        if (FAILED(m_dev->CreateShaderResourceView(picture, nullptr, &m_outSrv[slot]))) { Log("%s upscaler: the picture's view could not be made", kUpscalerName); return false; }
        m_outSrvFor[slot] = picture;
    }
    if (m_placeTarget != pass.out) {
        SafeRelease(m_placeUav); m_placeTarget = nullptr;
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{}; u.Format = Bridge::ViewFormat(pass.outFmt); u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(m_dev->CreateUnorderedAccessView(pass.out, &u, &m_placeUav))) { Log("%s upscaler: NIS's output could not be written in HDR (no UAV of format %d)", kUpscalerName, (int)u.Format); return false; }
        m_placeTarget = pass.out;
    }
    const uint32_t w = pass.outW, h = pass.outH;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_ctx->Map(m_placeConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    struct { uint32_t x, y, w, h, encoding; float white; float unused[2]; } constants = { pass.Partial() ? pass.outX : 0u, pass.Partial() ? pass.outY : 0u, w, h, m_encoding, m_white, { 0, 0 } };
    static_assert(sizeof constants == 32, "the place pass's cbuffer");
    memcpy(mapped.pData, &constants, sizeof constants); m_ctx->Unmap(m_placeConstants, 0);
    ID3D11Buffer* before = nullptr; m_ctx->CSGetConstantBuffers(0, 1, &before);
    m_ctx->CSSetShader(m_place, nullptr, 0);
    m_ctx->CSSetShaderResources(0, 1, &m_outSrv[slot]);
    m_ctx->CSSetUnorderedAccessViews(0, 1, &m_placeUav, nullptr);
    m_ctx->CSSetConstantBuffers(0, 1, &m_placeConstants);
    m_ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ID3D11ShaderResourceView* noSrv = nullptr; ID3D11UnorderedAccessView* noUav = nullptr;
    m_ctx->CSSetShaderResources(0, 1, &noSrv); m_ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    m_ctx->CSSetConstantBuffers(0, 1, &before);
    SafeRelease(before);
    return true;
}

void ScalerLink::PresentCopy(IDXGISwapChain* sc) {
    if (!m_atPresent || !m_dev || m_handoff != Handoff::AtPresent || m_encoding) return;   // (HDR pictures need the place pass: not at Present)
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    ID3D11Device* dev = nullptr; back->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC d{}; back->GetDesc(&d);
    const Shared& picture = m_out[m_atPresent % kOut];   // (a picture for part of the screen does not match the back buffer's size: not copied)
    if (dev == m_dev && d.Width == picture.w && d.Height == picture.h && m_holds[m_atPresent % kOut] == m_atPresent) {
        m_ctx->CopyResource(back, picture.d3d11);
        if (++m_copiedAtPresent == 1) Log("%s upscaler: first picture copied into the back buffer at Present", kUpscalerName);
        m_atPresent = 0;
    }
    if (dev) dev->Release();
    back->Release();
}

// Once per link, a while in: the average brightness (0-255) and the share of pure black pixels of the frame DLSS was given and the picture it
// made, read back without waiting (the copies are mapped only once the GPU has finished them).
void ScalerLink::Probe(uint64_t shown, bool inFresh) {
    if (m_probeState == 2 || !shown || m_holds[shown % kOut] != shown) return;
    auto bytes8 = [](DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM; };
    if (m_probeState == 0) {
        if (m_frame < 120 || !inFresh) return;
        const Shared* src[2] = { &m_in[m_frame % kIn], &m_out[shown % kOut] };
        for (int i = 0; i < 2; ++i) {
            D3D11_TEXTURE2D_DESC d{}; src[i]->d3d11->GetDesc(&d);
            d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.MiscFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(m_dev->CreateTexture2D(&d, nullptr, &m_probe[i]))) { SafeRelease(m_probe[0]); SafeRelease(m_probe[1]); m_probeState = 2; return; }
            m_ctx->CopyResource(m_probe[i], src[i]->d3d11);
        }
        m_probeState = 1;
        return;
    }
    const char* names[2] = { "frame given to the upscaler", "picture the upscaler made" };
    D3D11_MAPPED_SUBRESOURCE maps[2] = {};
    for (int i = 0; i < 2; ++i) {
        if (m_ctx->Map(m_probe[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &maps[i]) != S_OK) { if (i) m_ctx->Unmap(m_probe[0], 0); return; }   // not finished yet
    }
    for (int i = 0; i < 2; ++i) {
        D3D11_TEXTURE2D_DESC d{}; m_probe[i]->GetDesc(&d);
        if (!bytes8(d.Format)) { Log("%s upscaler: probe: the %s is format %d (not read)", kUpscalerName, names[i], (int)d.Format); continue; }
        double sum = 0; uint64_t n = 0, black = 0;
        for (uint32_t y = 0; y < d.Height; y += 16) {
            const uint8_t* row = static_cast<const uint8_t*>(maps[i].pData) + static_cast<size_t>(y) * maps[i].RowPitch;
            for (uint32_t x = 0; x < d.Width; x += 16) {
                const uint8_t* px = row + x * 4;
                const int v = px[0] + px[1] + px[2];
                sum += v / 3.0; ++n; if (v == 0) ++black;
            }
        }
        Log("%s upscaler: probe: the %s (%ux%u) averages %.1f of 255, %.1f%% of it pure black", kUpscalerName, names[i], d.Width, d.Height, n ? sum / n : 0.0, n ? 100.0 * black / n : 0.0);
    }
    for (int i = 0; i < 2; ++i) m_ctx->Unmap(m_probe[i], 0);
    SafeRelease(m_probe[0]); SafeRelease(m_probe[1]);
    m_probeState = 2;
}

} // namespace nr
