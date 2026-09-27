#include "addon/framegen11.h"
#include "addon/present_hook.h"
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
} g;
std::mutex g_mutex;

double Ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart; }

void ReleaseTextures() {
    SafeRelease(g.beforeSrv); SafeRelease(g.nowSrv); SafeRelease(g.before); SafeRelease(g.now);
    g.w = g.h = 0; g.format = DXGI_FORMAT_UNKNOWN; g.haveBefore = false;
}
void ReleaseAll() {
    ReleaseTextures(); SafeRelease(g.blend); SafeRelease(g.ctx); SafeRelease(g.dev);
    g.lastReal = {}; g.intervalMs = 0;
}

bool EnsureDevice(ID3D11Device* dev, const LogFn& log) {
    if (g.dev == dev && g.blend) return true;
    ReleaseAll();
    g.dev = dev; g.dev->AddRef(); g.dev->GetImmediateContext(&g.ctx);
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

    // the frame between, into the back buffer (the compute state Lossless Scaling had is put back after)
    ID3D11UnorderedAccessView* uav = nullptr;
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

void Reset() { std::lock_guard<std::mutex> lock(g_mutex); g.haveBefore = false; g.lastReal = {}; g.intervalMs = 0; }
void Shutdown() { std::lock_guard<std::mutex> lock(g_mutex); ReleaseAll(); if (g.timer) { CloseHandle(g.timer); g.timer = nullptr; } }
Stats GetStats() { std::lock_guard<std::mutex> lock(g_mutex); return g.stats; }

} // namespace nr::framegen
