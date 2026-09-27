#include "addon/framegen11.h"
#include "addon/hdr.h"
#include "addon/present_hook.h"
#include "engine/fg_engine.h"
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

namespace nr::framegen {
namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Step 1's frame between: the frame before and this one, half and half, written straight into the back buffer (Lossless Scaling makes its
// back buffers with unordered access: its NIS pass writes them from a compute shader).
const char* const kBlendHlsl = R"(
Texture2D<float4> tBefore : register(t0);
Texture2D<float4> tNow : register(t1);
RWTexture2D<float4> uOut : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h; uOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    uOut[id.xy] = 0.5 * (tBefore[id.xy] + tNow[id.xy]);
}
)";

struct State {
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; ID3D11ComputeShader* blend = nullptr;
    ID3D11Texture2D* before = nullptr; ID3D11Texture2D* now = nullptr;               // the real frame before, and this one
    ID3D11ShaderResourceView* beforeSrv = nullptr; ID3D11ShaderResourceView* nowSrv = nullptr;
    UINT w = 0, h = 0; DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; bool haveBefore = false;
    LARGE_INTEGER lastReal{}; double intervalMs = 0;                                   // between real frames, smoothed
    HANDLE timer = nullptr;
    Stats stats;
    // FSR 3.1 frame generation: the frame in and the frame made, shared with FgEngine's device, and a fence for the copy in
    FgEngine engine; std::wstring runtime; bool engineFailed = false;
    ID3D11Texture2D* in11 = nullptr; ID3D11Texture2D* out11 = nullptr; ID3D12Resource* in12 = nullptr; ID3D12Resource* out12 = nullptr;
    ID3D11Fence* copied11 = nullptr; ID3D12Fence* copied12 = nullptr; uint64_t copiedValue = 0;
    ID3D11DeviceContext4* ctx4 = nullptr;
    bool resetNext = true;
} g;
std::mutex g_mutex;

double Ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart; }

void ReleaseShared() {
    g.engine.Shutdown();
    SafeRelease(g.in12); SafeRelease(g.out12); SafeRelease(g.copied12); SafeRelease(g.in11); SafeRelease(g.out11); SafeRelease(g.copied11);
    g.copiedValue = 0; g.resetNext = true;
}
void ReleaseTextures() {
    ReleaseShared();
    SafeRelease(g.beforeSrv); SafeRelease(g.nowSrv); SafeRelease(g.before); SafeRelease(g.now);
    g.w = g.h = 0; g.format = DXGI_FORMAT_UNKNOWN; g.haveBefore = false; g.engineFailed = false;
}
void ReleaseAll() {
    ReleaseTextures(); SafeRelease(g.blend); SafeRelease(g.ctx4); SafeRelease(g.ctx); SafeRelease(g.dev);
    g.lastReal = {}; g.intervalMs = 0;
}

bool EnsureDevice(ID3D11Device* dev, const LogFn& log) {
    if (g.dev == dev && g.blend) return true;
    ReleaseAll();
    g.dev = dev; g.dev->AddRef(); g.dev->GetImmediateContext(&g.ctx); g.ctx->QueryInterface(IID_PPV_ARGS(&g.ctx4));
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(kBlendHlsl, strlen(kBlendHlsl), "framegen_blend", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        if (log) log(err ? static_cast<const char*>(err->GetBufferPointer()) : "frame generation: the blend shader did not compile");
        SafeRelease(err); return false;
    }
    const HRESULT hr = g.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g.blend);
    SafeRelease(code);
    return SUCCEEDED(hr);
}

bool EnsureTextures(const D3D11_TEXTURE2D_DESC& back, const LogFn& log) {
    if (g.before && g.w == back.Width && g.h == back.Height && g.format == back.Format) return true;
    ReleaseTextures();
    D3D11_TEXTURE2D_DESC d{}; d.Width = back.Width; d.Height = back.Height; d.MipLevels = 1; d.ArraySize = 1; d.Format = back.Format;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &g.before)) || FAILED(g.dev->CreateTexture2D(&d, nullptr, &g.now)) ||
        FAILED(g.dev->CreateShaderResourceView(g.before, nullptr, &g.beforeSrv)) || FAILED(g.dev->CreateShaderResourceView(g.now, nullptr, &g.nowSrv))) {
        if (log) { char m[128]; snprintf(m, sizeof m, "frame generation: frames of format %d cannot be kept", static_cast<int>(back.Format)); log(m); }
        ReleaseTextures(); return false;
    }
    g.w = back.Width; g.h = back.Height; g.format = back.Format;
    if (log) { char m[128]; snprintf(m, sizeof m, "frame generation: %ux%u frames, format %d", back.Width, back.Height, static_cast<int>(back.Format)); log(m); }
    return true;
}

// FgEngine and the textures and fence it shares with Lossless Scaling's device, for frames like the back buffer. False: the blend stands in.
bool EnsureEngine(IDXGISwapChain* sc, const D3D11_TEXTURE2D_DESC& back, const LogFn& log) {
    if (g.engineFailed) return false;
    IDXGIDevice* dxgi = nullptr; IDXGIAdapter* adapter = nullptr; DXGI_ADAPTER_DESC ad{};
    if (FAILED(g.dev->QueryInterface(IID_PPV_ARGS(&dxgi)))) return false;
    dxgi->GetAdapter(&adapter); dxgi->Release();
    if (!adapter) return false;
    adapter->GetDesc(&ad); adapter->Release();
    const bool hdr = back.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (g.engine.Matches(ad.AdapterLuid, back.Width, back.Height, back.Format, hdr) && g.in12 && g.out12 && g.copied12) return true;
    ReleaseShared();
    auto fail = [&](const char* why) { if (log) { std::string m = std::string("frame generation: ") + why + "; the blend stands in"; log(m.c_str()); } ReleaseShared(); g.engineFailed = true; return false; };
    if (g.runtime.empty() || !g.ctx4) return fail("no FSR runtime, or no Direct3D 11.4");
    if (!g.engine.Init(ad.AdapterLuid, g.runtime, back.Width, back.Height, back.Format, hdr, nr::QueryDisplayHdr(sc, g.dev).whiteNits, log)) return fail(g.engine.LastError());
    ID3D11Device5* dev5 = nullptr; g.dev->QueryInterface(IID_PPV_ARGS(&dev5));
    if (!dev5 || FAILED(dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.copied11)))) { SafeRelease(dev5); return fail("the shared fence could not be made"); }
    dev5->Release();
    HANDLE h = nullptr;
    if (FAILED(g.copied11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h))) return fail("the fence could not be shared");
    g.copied12 = g.engine.OpenSharedFence(h); CloseHandle(h);
    auto shared = [&](ID3D11Texture2D*& t11, ID3D12Resource*& t12) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = back.Width; d.Height = back.Height; d.MipLevels = 1; d.ArraySize = 1; d.Format = back.Format;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &t11))) return false;
        IDXGIResource1* r = nullptr; HANDLE th = nullptr;
        if (FAILED(t11->QueryInterface(IID_PPV_ARGS(&r)))) return false;
        const HRESULT hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &th); r->Release();
        if (FAILED(hr)) return false;
        t12 = g.engine.OpenSharedTexture(th); CloseHandle(th);
        return t12 != nullptr;
    };
    if (!g.copied12 || !shared(g.in11, g.in12) || !shared(g.out11, g.out12)) return fail("the shared frames could not be made");
    g.resetNext = true;
    return true;
}

// Waits until `due` on the QPC clock: a high-resolution timer for most of it, then a short spin (a Sleep alone overshoots by a millisecond or more)
void WaitUntil(const LARGE_INTEGER& due) {
    if (!g.timer) g.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (;;) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        const double left = Ms(now, due);
        if (left <= 0.0) return;
        if (left > 1.5 && g.timer) {
            LARGE_INTEGER rel; rel.QuadPart = -static_cast<LONGLONG>((left - 1.0) * 10000.0);   // 100 ns units, relative
            if (SetWaitableTimerEx(g.timer, &rel, 0, nullptr, nullptr, nullptr, 0)) { WaitForSingleObject(g.timer, 100); continue; }
        }
        YieldProcessor();
    }
}

} // namespace

bool BeforeRealPresent(IDXGISwapChain* sc, UINT sync, UINT flags, const LogFn& log) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LARGE_INTEGER arrived; QueryPerformanceCounter(&arrived);
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
    D3D11_TEXTURE2D_DESC bd{}; back->GetDesc(&bd);
    ID3D11Device* dev = nullptr; back->GetDevice(&dev);
    const bool usable = bd.SampleDesc.Count == 1 && (bd.BindFlags & D3D11_BIND_UNORDERED_ACCESS) && dev && EnsureDevice(dev, log) && EnsureTextures(bd, log);
    SafeRelease(dev);
    if (!usable) { back->Release(); return false; }

    // the time between real frames (smoothed; a pause, such as a loading screen, starts it again)
    if (g.lastReal.QuadPart) {
        const double dt = Ms(g.lastReal, arrived);
        if (dt > 2.0 && dt < 100.0) g.intervalMs = g.intervalMs > 0 && dt < 3.0 * g.intervalMs ? 0.9 * g.intervalMs + 0.1 * dt : dt;
        else g.haveBefore = false;
    }
    g.lastReal = arrived;
    g.ctx->CopyResource(g.now, back);
    if (!g.haveBefore || g.intervalMs <= 0) {   // nothing to go between yet: this frame becomes the one before
        std::swap(g.before, g.now); std::swap(g.beforeSrv, g.nowSrv); g.haveBefore = true;
        back->Release(); ++g.stats.real; return false;
    }

    // the frame between: FSR's (the frame copied over to FgEngine, the frame made copied back), or the blend
    bool byFsr = false;
    if (EnsureEngine(sc, bd, log)) {
        LARGE_INTEGER f0; QueryPerformanceCounter(&f0);
        g.ctx->CopyResource(g.in11, back);
        g.ctx4->Signal(g.copied11, ++g.copiedValue);
        g.ctx->Flush();   // the copy and the signal on the GPU's way before the engine waits for them
        byFsr = g.engine.Generate(g.in12, g.copied12, g.copiedValue, g.out12, static_cast<float>(g.intervalMs), g.resetNext);
        g.resetNext = false;
        if (byFsr) {
            g.ctx->CopyResource(back, g.out11);
            LARGE_INTEGER f1; QueryPerformanceCounter(&f1);
            ++g.stats.byFsr; g.stats.fsrMs = 0.9 * g.stats.fsrMs + 0.1 * Ms(f0, f1);
        }
    }
    g.stats.engine = g.engine.IsReady() ? "FSR 3.1" : g.engineFailed ? std::string("blend (") + g.engine.LastError() + ")" : "blend";
    ID3D11UnorderedAccessView* uav = nullptr;
    if (!byFsr) {
    if (FAILED(g.dev->CreateUnorderedAccessView(back, nullptr, &uav))) {
        if (log) { char m[128]; snprintf(m, sizeof m, "frame generation: no unordered access to a back buffer of format %d", static_cast<int>(bd.Format)); log(m); }
        back->Release(); return false;
    }
    ID3D11ComputeShader* oldCs = nullptr; ID3D11ClassInstance* oldInst[16] = {}; UINT oldInstCount = 16;
    ID3D11ShaderResourceView* oldSrv[2] = {}; ID3D11UnorderedAccessView* oldUav = nullptr;
    g.ctx->CSGetShader(&oldCs, oldInst, &oldInstCount); g.ctx->CSGetShaderResources(0, 2, oldSrv); g.ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);
    ID3D11ShaderResourceView* srvs[2] = { g.beforeSrv, g.nowSrv };
    g.ctx->CSSetShader(g.blend, nullptr, 0); g.ctx->CSSetShaderResources(0, 2, srvs); g.ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    g.ctx->Dispatch((bd.Width + 7) / 8, (bd.Height + 7) / 8, 1);
    g.ctx->CSSetShader(oldCs, oldInst, oldInstCount); g.ctx->CSSetShaderResources(0, 2, oldSrv); g.ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, nullptr);
    SafeRelease(oldCs); for (UINT i = 0; i < oldInstCount; ++i) SafeRelease(oldInst[i]); SafeRelease(oldSrv[0]); SafeRelease(oldSrv[1]); SafeRelease(oldUav);
    SafeRelease(uav);
    }
    back->Release();

    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    PresentHook::PresentOriginal(sc, sync, flags);   // the frame between goes out now...
    LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
    ++g.stats.generated; g.stats.generatedMs = 0.9 * g.stats.generatedMs + 0.1 * Ms(t0, t1);
    // ...and the real frame half a frame later, back in the swap chain's next back buffer
    LARGE_INTEGER due; due.QuadPart = arrived.QuadPart;
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); due.QuadPart += static_cast<LONGLONG>(g.intervalMs * 0.5 * f.QuadPart / 1000.0); }
    WaitUntil(due);
    ID3D11Texture2D* next = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&next))) && next) { g.ctx->CopyResource(next, g.now); next->Release(); }
    std::swap(g.before, g.now); std::swap(g.beforeSrv, g.nowSrv);
    ++g.stats.real; g.stats.realIntervalMs = g.intervalMs;
    return true;
}

void SetRuntime(const std::wstring& dll) { std::lock_guard<std::mutex> lock(g_mutex); g.runtime = dll; }
void Reset() { std::lock_guard<std::mutex> lock(g_mutex); g.haveBefore = false; g.lastReal = {}; g.intervalMs = 0; g.resetNext = true; }
void Shutdown() { std::lock_guard<std::mutex> lock(g_mutex); ReleaseAll(); if (g.timer) { CloseHandle(g.timer); g.timer = nullptr; } }
Stats GetStats() { std::lock_guard<std::mutex> lock(g_mutex); return g.stats; }

} // namespace nr::framegen
