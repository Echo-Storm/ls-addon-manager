#include "addon/framegen11.h"
#include "addon/present_hook.h"
#include "engine/fg_engine.h"
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <thread>
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
    // how long after a real frame arrives the frame between goes out: the time making it takes (its recent peak, slowly let down), so the
    // frame between and the real one half a frame after it go out evenly spaced whatever each frame's making took
    double leadMs = 0;
    HANDLE timer = nullptr;
    Stats stats;
    LARGE_INTEGER saidAt{}; Stats said;                                                  // for the log, every 10 s
    // FSR 3.1 frame generation: the frame in and the frame made, shared with FgEngine's device, and a fence for the copy in
    FgEngine engine; std::wstring runtime; bool engineFailed = false;
    // the engine starts on a thread of its own (FSR's runtime, the motion estimate's and EchoFG's shaders: a second or two): until it is ready the frames between are the blend
    std::thread initThread; std::atomic<int> initState{ 0 }; std::string initError;   // initState: 0 idle, 1 starting, 2 started, 3 failed
    ID3D11Texture2D* in11 = nullptr; ID3D11Texture2D* out11 = nullptr; ID3D12Resource* in12 = nullptr; ID3D12Resource* out12 = nullptr;
    ID3D11Fence* copied11 = nullptr; ID3D12Fence* copied12 = nullptr; uint64_t copiedValue = 0;
    ID3D11Fence* made11 = nullptr; ID3D12Fence* made12 = nullptr; uint64_t madeValue = 0;   // the frame between is in out11 (a GPU wait here)
    ID3D11DeviceContext4* ctx4 = nullptr;
    bool resetNext = true; std::atomic<bool> guard{ true }, own{ false }; std::atomic<int> multiplier{ 2 }; std::atomic<float> soften{ 0.6f };   // multiplier: frames shown for each real one (more than 2 only with our own synthesis)
    ID3D11Texture2D* more11[OwnSynth::kMaxTargets] = {}; ID3D12Resource* more12[OwnSynth::kMaxTargets] = {};   // the 2nd, 3rd... frame between (the first is out11)
    // what the display did with the presents (DXGI's frame statistics): the refreshes between two consecutive presents, counted, for the log (a steady x2 is all ones)
    UINT lastPresentCount = 0, lastRefreshCount = 0; uint64_t refreshGaps[6] = {}, presentsSeenOnce = 0, presentsSkipped = 0, statsFailed = 0;
    // for the log: how long Lossless Scaling's thread is held here, and how soon after it comes back with the next real frame
    LARGE_INTEGER returned{}; double heldSum = 0, freeSum = 0; uint64_t heldCount = 0, freeCount = 0;
} g;
std::mutex g_mutex;                 // the presenter's state: held through a call of BeforeRealPresent (its waits included)
std::mutex g_statsMutex; Stats g_published;   // what the panel reads: a copy made at two points of each call, under a lock of its own
void Publish() { std::lock_guard<std::mutex> lock(g_statsMutex); g_published = g.stats; }

// What the display has shown so far: when a present has come on screen since the last look, how many refreshes after the one before. (Windowed flip-model swap chains report this too.)
void SampleDisplay(IDXGISwapChain* sc) {
    DXGI_FRAME_STATISTICS fs{};
    if (FAILED(sc->GetFrameStatistics(&fs))) { ++g.statsFailed; return; }
    if (fs.PresentCount == g.lastPresentCount) return;
    if (g.lastPresentCount) {
        const UINT presents = fs.PresentCount - g.lastPresentCount;
        if (presents == 1) { const UINT gap = fs.PresentRefreshCount - g.lastRefreshCount; ++g.refreshGaps[std::min<UINT>(gap, 5)]; ++g.presentsSeenOnce; }
        else g.presentsSkipped += presents - 1;   // (looked too late to see each one: counted, not timed)
    }
    g.lastPresentCount = fs.PresentCount; g.lastRefreshCount = fs.PresentRefreshCount;
}

double Ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart; }

void JoinInit() { if (g.initThread.joinable()) g.initThread.join(); }

void ReleaseShared() {
    JoinInit(); g.initState = 0;
    g.engine.Shutdown();
    SafeRelease(g.in12); SafeRelease(g.out12); SafeRelease(g.copied12); SafeRelease(g.in11); SafeRelease(g.out11); SafeRelease(g.copied11);
    SafeRelease(g.made12); SafeRelease(g.made11);
    for (auto*& r : g.more11) SafeRelease(r);
    for (auto*& r : g.more12) SafeRelease(r);
    g.copiedValue = 0; g.madeValue = 0; g.resetNext = true;
}
void ReleaseTextures() {
    ReleaseShared();
    SafeRelease(g.beforeSrv); SafeRelease(g.nowSrv); SafeRelease(g.before); SafeRelease(g.now);
    g.w = g.h = 0; g.format = DXGI_FORMAT_UNKNOWN; g.haveBefore = false; g.engineFailed = false;
}
void ReleaseAll() {
    ReleaseTextures(); SafeRelease(g.blend); SafeRelease(g.ctx4); SafeRelease(g.ctx); SafeRelease(g.dev);
    g.lastReal = {}; g.intervalMs = 0; g.leadMs = 0;
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

// A texture of the frames' kind shared with FgEngine's device: made on Lossless Scaling's, opened on the engine's.
bool MakeShared(const D3D11_TEXTURE2D_DESC& back, ID3D11Texture2D*& t11, ID3D12Resource*& t12) {
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
}

// The two shared fences, "copied" (the frame is in, for the engine's queue) and "made" (the frame between is out, for Lossless Scaling's), and the shared frames: in, out,
// and `extras` more for a multiplier. Null when all was made, else why not. Runs on the engine's start-up thread (the device is free-threaded; nothing here uses the immediate context).
const char* MakeShares(const D3D11_TEXTURE2D_DESC& back, int extras) {
    ID3D11Device5* dev5 = nullptr; g.dev->QueryInterface(IID_PPV_ARGS(&dev5));
    if (!dev5 || FAILED(dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.copied11))) || FAILED(dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.made11)))) {
        SafeRelease(dev5); return "the shared fences could not be made";
    }
    dev5->Release();
    auto shareFence = [&](ID3D11Fence* f11, ID3D12Fence*& f12) {
        HANDLE h = nullptr;
        if (FAILED(f11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h))) return false;
        f12 = g.engine.OpenSharedFence(h); CloseHandle(h);
        return f12 != nullptr;
    };
    if (!shareFence(g.copied11, g.copied12) || !shareFence(g.made11, g.made12)) return "the fences could not be shared";
    if (!MakeShared(back, g.in11, g.in12) || !MakeShared(back, g.out11, g.out12)) return "the shared frames could not be made";
    for (int k = 1; k <= extras && k < OwnSynth::kMaxTargets; ++k) if (!g.more12[k] && !MakeShared(back, g.more11[k], g.more12[k])) { SafeRelease(g.more11[k]); SafeRelease(g.more12[k]); break; }
    { ID3D12Resource* outs[OwnSynth::kMaxTargets] = { g.out12 }; for (int k = 1; k < OwnSynth::kMaxTargets; ++k) outs[k] = g.more12[k]; g.engine.Warm(g.in12, outs, OwnSynth::kMaxTargets); }   // (the first use of shared frames is slow at 4K: here, not on the presenting thread)   // (a multiplier asks for them again when it needs them)
    return nullptr;
}

// FgEngine and the textures and fence it shares with Lossless Scaling's device, for frames like the back buffer. The engine starts on a thread of its own and this says false
// (the blend stands in) until it is ready; a start that failed stays failed until the frames' size or format changes.
bool EnsureEngine(const D3D11_TEXTURE2D_DESC& back, const LogFn& log) {
    if (g.engineFailed) return false;
    if (g.initState == 1) return false;   // still starting
    if (g.initThread.joinable()) g.initThread.join();   // (it has finished)
    auto fail = [&](const char* why) { if (log) { std::string m = std::string("frame generation: ") + why + "; the blend stands in"; log(m.c_str()); } ReleaseShared(); g.engineFailed = true; return false; };
    if (g.initState == 3) return fail(g.initError.c_str());
    IDXGIDevice* dxgi = nullptr; IDXGIAdapter* adapter = nullptr; DXGI_ADAPTER_DESC ad{};
    if (FAILED(g.dev->QueryInterface(IID_PPV_ARGS(&dxgi)))) return false;
    dxgi->GetAdapter(&adapter); dxgi->Release();
    if (!adapter) return false;
    adapter->GetDesc(&ad); adapter->Release();
    const bool hdr = back.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!g.engine.Matches(ad.AdapterLuid, back.Width, back.Height, back.Format, hdr)) {
        // no engine for these frames (the first, or a new size or format): the old one goes, a new one starts beside the frames
        ReleaseShared();
        if (g.runtime.empty() || !g.ctx4) return fail("no FSR runtime, or no Direct3D 11.4");
        g.engine.SetOwnSynthesis(g.own.load());   // (EchoFG's shaders compile in the same start-up when it is wanted already)
        g.initState = 1;
        const LUID luid = ad.AdapterLuid; const std::wstring runtime = g.runtime; const UINT w = back.Width, h = back.Height; const DXGI_FORMAT fmt = back.Format; const LogFn logCopy = log;
        const D3D11_TEXTURE2D_DESC desc = back; const int extras = std::clamp(g.multiplier.load(), 2, OwnSynth::kMaxTargets + 1) - 2;
        g.initThread = std::thread([luid, runtime, w, h, fmt, hdr, logCopy, desc, extras] {
            bool ok = g.engine.Init(luid, runtime, w, h, fmt, hdr, logCopy);
            if (!ok) g.initError = g.engine.LastError();
            else if (const char* why = MakeShares(desc, extras)) { g.initError = why; ok = false; }   // (the shared frames and fences with it: nothing is left to make on the presenting thread)
            g.initState = ok ? 2 : 3;
        });
        return false;
    }
    if (g.in12 && g.out12 && g.copied12 && g.made12) return true;
    if (const char* why = MakeShares(back, 0)) return fail(why);   // (the engine is up but the shared frames are gone: made again here)
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

bool BeforeRealPresent(IDXGISwapChain* sc, UINT sync, UINT flags, uint32_t encoding, float whiteNits, const LogFn& log, const ShownFn& shown) {
    std::lock_guard<std::mutex> lock(g_mutex);
    LARGE_INTEGER arrived; QueryPerformanceCounter(&arrived);
    if (g.returned.QuadPart) {   // how soon Lossless Scaling came back after it was let go (small: it had a frame waiting, held up by us)
        const double free = Ms(g.returned, arrived);
        if (free < 100.0) { g.freeSum += free; ++g.freeCount; }
        g.returned = {};
    }
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
        else { g.haveBefore = false; g.resetNext = true; }   // (a pause, a double present: the frame before is not this one's)
    }
    g.lastReal = arrived;
    g.ctx->CopyResource(g.now, back);
    if (!g.haveBefore || g.intervalMs <= 0) {   // nothing to go between yet: this frame becomes the one before
        std::swap(g.before, g.now); std::swap(g.beforeSrv, g.nowSrv); g.haveBefore = true;
        if (shown) shown(g.ctx, back, false);
        back->Release(); ++g.stats.real; return false;
    }

    // the frame between: FSR's (the frame copied over to FgEngine, the frame made copied back), or the blend
    bool byFsr = false; int made = 0;   // made: how many frames between the engine made (0 with the blend)
    LARGE_INTEGER stageA; QueryPerformanceCounter(&stageA);
    const bool engineUp = EnsureEngine(bd, log);
    LARGE_INTEGER stageB; QueryPerformanceCounter(&stageB);
    LARGE_INTEGER stageFlushed = stageB, stageCopied = stageB, stageSignalled = stageB;
    if (engineUp) {
        g.ctx->CopyResource(g.in11, back);
        QueryPerformanceCounter(&stageCopied);
        g.ctx4->Signal(g.copied11, ++g.copiedValue);
        QueryPerformanceCounter(&stageSignalled);
        g.ctx->Flush();   // the copy and the signal on the GPU's way before the engine waits for them
        QueryPerformanceCounter(&stageFlushed);
        // queued, not waited for: Lossless Scaling's queue waits on the GPU for "made" before the frame between is copied in
        g.engine.SetOwnSynthesis(g.own.load()); g.engine.SetSoften(g.soften.load());
        // frames between wanted: the multiplier less one, when our own synthesis can make that many (it is known to after its first frame); else the one at half way
        int want = g.own ? std::clamp(g.multiplier.load(), 2, OwnSynth::kMaxTargets + 1) - 1 : 1;
        want = std::min(want, g.engine.MaxFramesBetween());
        for (int k = 1; k < want; ++k) if (!g.more12[k] && !MakeShared(bd, g.more11[k], g.more12[k])) { SafeRelease(g.more11[k]); SafeRelease(g.more12[k]); want = k; break; }
        ID3D12Resource* outs[OwnSynth::kMaxTargets]; outs[0] = g.out12; for (int k = 1; k < want; ++k) outs[k] = g.more12[k];
        made = g.engine.GenerateFrames(g.in12, g.copied12, g.copiedValue, outs, want, g.made12, ++g.madeValue, static_cast<float>(g.intervalMs), g.resetNext, encoding, whiteNits, g.guard.load());
        g.resetNext = false;
        LARGE_INTEGER stageC; QueryPerformanceCounter(&stageC);
        { static int said = 0; const double ensureMs = Ms(stageA, stageB), engineMs = Ms(stageB, stageC);
          if ((ensureMs > 40.0 || engineMs > 40.0) && said < 6 && log) { ++said; char m[200]; snprintf(m, sizeof m, "frame generation: a slow frame on the presenting thread: the engine check %.0f ms, the engine's frame %.0f ms (copying the frame over %.0f ms: the copy %.0f, the signal %.0f, the flush %.0f)", ensureMs, engineMs, Ms(stageB, stageFlushed), Ms(stageB, stageCopied), Ms(stageCopied, stageSignalled), Ms(stageSignalled, stageFlushed)); log(m); } }
        byFsr = made > 0;
        if (g.engine.IsReady()) g.ctx4->Wait(g.made11, g.madeValue);   // (the engine signals it on every path: the next copy into in11 must follow its reading of it, also when it made none)
        if (byFsr) {
            g.ctx->CopyResource(back, g.out11);
            g.stats.byFsr += made; g.stats.fsrMs = g.engine.GpuMs();
        }
    }
    g.stats.engine = g.engine.IsReady() ? (g.engine.UsingOwnSynthesis() ? "EchoFG" : "FSR 3.1 (EchoFG motion)") : g.engineFailed ? std::string("blend (") + g.engine.LastError() + ")" : "blend";
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
    if (shown) shown(g.ctx, back, true);
    back->Release();

    // The frame between goes out now; the GPU shows it once it is made (its wait above), about `leadMs` after the real frame arrived: the
    // GPU's time for it (or the CPU's, for the blend). The real frame goes out half a frame after that, so the two are evenly spaced.
    LARGE_INTEGER madeAt; QueryPerformanceCounter(&madeAt);
    const double work = std::max(Ms(arrived, madeAt), byFsr ? g.engine.GpuMs() + 0.5 : 0.0);
    const int frames = std::max(1, made);
    // (at most one frame's share of the interval: the real frame is let go at lead + frames / (frames + 1) of it, and the thread must be back before the next real frame arrives)
    g.leadMs = std::min(std::max(work, 0.97 * g.leadMs + 0.03 * work), std::max(0.0, g.intervalMs / (frames + 1) - 1.0));
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    auto after = [&](double ms) { LARGE_INTEGER t; t.QuadPart = arrived.QuadPart + static_cast<LONGLONG>(ms * f.QuadPart / 1000.0); return t; };
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    SampleDisplay(sc);
    PresentHook::PresentOriginal(sc, sync, flags);   // the frame between goes out now...
    SampleDisplay(sc);
    LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
    ++g.stats.generated; g.stats.generatedMs = 0.9 * g.stats.generatedMs + 0.1 * Ms(t0, t1);
    Publish();
    // ...the others, if a multiplier asked for more, each in the next back buffer at its own time, evenly spaced...
    for (int k = 1; k < frames; ++k) {
        WaitUntil(after(g.leadMs + k * g.intervalMs / (frames + 1)));
        ID3D11Texture2D* nb = nullptr;
        if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&nb))) || !nb) break;
        g.ctx->CopyResource(nb, g.more11[k]);
        if (shown) shown(g.ctx, nb, true);
        nb->Release();
        SampleDisplay(sc);
        PresentHook::PresentOriginal(sc, sync, flags);
        SampleDisplay(sc);
        ++g.stats.generated;
    }
    // ...and the real frame after them, back in the swap chain's next back buffer
    WaitUntil(after(g.leadMs + frames * g.intervalMs / (frames + 1)));
    if (log && Ms(g.saidAt, t1) >= 10000.0) {   // what it did, now and then
        if (g.saidAt.QuadPart) {
            char m[360];
            snprintf(m, sizeof m, "frame generation: %llu real frames %.1f ms apart, %llu made between (%llu by %s, the GPU %.1f ms each; shown %.1f ms after the real frame "
                     "arrives; its present %.2f ms); Lossless Scaling's thread held %.1f ms a frame, back %.1f ms after",
                     static_cast<unsigned long long>(g.stats.real - g.said.real), g.intervalMs, static_cast<unsigned long long>(g.stats.generated - g.said.generated),
                     static_cast<unsigned long long>(g.stats.byFsr - g.said.byFsr), g.stats.engine.c_str(), g.stats.fsrMs, g.leadMs, g.stats.generatedMs,
                     g.heldCount ? g.heldSum / g.heldCount : 0.0, g.freeCount ? g.freeSum / g.freeCount : 0.0);
            log(m);
            if (g.presentsSeenOnce + g.presentsSkipped > 0) {   // the display's side: refreshes between consecutive presents (0 = shown within the same refresh, a tear or a replaced frame; 1 = every refresh)
                char d[300];
                snprintf(d, sizeof d, "frame generation: the display showed %llu presents one by one, refreshes after the present before: 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu 5+:%llu; %llu more were not seen one by one%s",
                         static_cast<unsigned long long>(g.presentsSeenOnce), static_cast<unsigned long long>(g.refreshGaps[0]), static_cast<unsigned long long>(g.refreshGaps[1]), static_cast<unsigned long long>(g.refreshGaps[2]),
                         static_cast<unsigned long long>(g.refreshGaps[3]), static_cast<unsigned long long>(g.refreshGaps[4]), static_cast<unsigned long long>(g.refreshGaps[5]), static_cast<unsigned long long>(g.presentsSkipped),
                         g.statsFailed ? " (the statistics were refused at times)" : "");
                log(d);
            }
            memset(g.refreshGaps, 0, sizeof g.refreshGaps); g.presentsSeenOnce = g.presentsSkipped = g.statsFailed = 0;
        }
        g.saidAt = t1; g.said = g.stats; g.heldSum = g.freeSum = 0; g.heldCount = g.freeCount = 0;
    }
    ID3D11Texture2D* next = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&next))) && next) { g.ctx->CopyResource(next, g.now); if (shown) shown(g.ctx, next, false); next->Release(); }
    std::swap(g.before, g.now); std::swap(g.beforeSrv, g.nowSrv);
    ++g.stats.real; g.stats.realIntervalMs = g.intervalMs;
    Publish();
    QueryPerformanceCounter(&g.returned);
    g.heldSum += Ms(arrived, g.returned); ++g.heldCount;
    return true;
}

void SetRuntime(const std::wstring& dll) { std::lock_guard<std::mutex> lock(g_mutex); g.runtime = dll; }
void SetGuard(bool on) { g.guard = on; }   // (no lock: the presenter holds its own through its waits)
void SetOwn(bool on) { g.own = on; }
void SetSoften(float amount) { g.soften = std::clamp(amount, 0.0f, 1.0f); }
void SetMultiplier(int n) { g.multiplier = std::clamp(n, 2, OwnSynth::kMaxTargets + 1); }
void Reset() { std::lock_guard<std::mutex> lock(g_mutex); g.haveBefore = false; g.lastReal = {}; g.intervalMs = 0; g.leadMs = 0; g.resetNext = true; }
void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    ReleaseAll(); if (g.timer) { CloseHandle(g.timer); g.timer = nullptr; }
    g.stats = {}; g.said = {}; g.saidAt = {}; g.returned = {};
    Publish();
}
Stats GetStats() { std::lock_guard<std::mutex> lock(g_statsMutex); return g_published; }

} // namespace nr::framegen
