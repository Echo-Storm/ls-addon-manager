// nr_fgpresent: the frame generation presenter (src/addon/framegen11.cpp) on a real swap chain, without Lossless Scaling.
//
// A window's flip-model swap chain on a Direct3D 11 device (back buffers with unordered access, as Lossless Scaling makes them), the addon's Present hook patched onto it, and the
// presenter called from the hook exactly as the addon does (framegen::BeforeRealPresent). Real frames are a moving white square on a textured background, drawn into the
// back buffer by a compute shader at a steady rate; the presenter makes the frames between with FgEngine (our own synthesis, or FSR 3.1) and shows them through the same swap chain.
// Every frame shown (the callback the recorder uses) is copied and read back after the run, and the square's position in each is checked against where it should be:
//   * how many frames came out per real frame (the multiplier),
//   * their order (the made ones in the interval, then the real one),
//   * the square's x against x0 + v (n - 1 + k / N) for made frame k of N in the interval ending at real frame n,
//   * the spacing in time of the frames shown.
//   nr_fgpresent [mult=4] [hdr=0|1] [frames=30] [own=1] [fps=30] [speed=24] [fsr=<amd_fidelityfx_dx12.dll>]
//                [dup=2]     every real frame comes dup times (a 30 fps game in a 60 Hz feed: with fps=60): the frames made between identical ones must be the same too
//                [switch=1]  the multiplier changed while it runs (to x2 a third of the way, to x5 two thirds): the last intervals must have the last one's count
//                [toggle=1]  starts with FSR (own=0) and switches EchoFG on half way: neither the start-up nor the switch may stall the presenting thread
//                [resize=1]  the swap chain's buffers resized to 800x450 half way: the presenter starts again at the new size and goes on
//   How the swap chain is presented (the display timing study, board E3: what the owner's Lossless Scaling settings do to the frames' present):
//                [sync=0|1|2]   the sync interval of every Present (Lossless Scaling's Sync mode)
//                [tearing=1]    the swap chain allows tearing, and a Present with sync=0 asks for it ("Off (allow tearing)")
//                [latency=N]    a waitable swap chain with maximum frame latency N: the thread waits on it before each frame (Lossless Scaling's "Max frame latency")
//                [buffers=N]    the number of back buffers (2..6, default 3)
//                [align=0|1]    the presenter's "one present to a refresh" (default 1); the 10 second log line says what the display did with the presents (needs frames long enough: fps=30 frames=400)
//                [size=2|4]     the picture 1920x1080 or 3840x2160 instead of 960x540
//               It then prints how long each Present call took (the real frame's includes the presenter's work and its waits; the made frames' alone) and the display's present count.
// Needs a build made with NR_FRAMEGEN=ON only for the addon itself; this tool links the presenter directly, so it runs in any build.
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "addon/framegen11.h"
#include "addon/present_hook.h"
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

int Arg(int argc, char** argv, const char* key, int fallback) {
    const size_t k = strlen(key);
    for (int i = 1; i < argc; ++i) if (!strncmp(argv[i], key, k) && argv[i][k] == '=') return atoi(argv[i] + k + 1);
    return fallback;
}
std::string ArgText(int argc, char** argv, const char* key) {
    const size_t k = strlen(key);
    for (int i = 1; i < argc; ++i) if (!strncmp(argv[i], key, k) && argv[i][k] == '=') return argv[i] + k + 1;
    return {};
}

const char* const kScene = R"(
RWTexture2D<float4> o : register(u0);
cbuffer C : register(b0) { float sx; float sy; float scale; float pad; };
float Hash(float2 c) { return frac(sin(dot(c, float2(12.9898, 78.233))) * 43758.5453); }
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
    const float2 p = float2(id.xy);
    const float bg = 0.22 + 0.12 * sin(p.x * 0.11) * sin(p.y * 0.13) + 0.1 * Hash(floor(p / 4.0));
    const bool inside = p.x >= sx && p.x < sx + 96.0 && p.y >= sy && p.y < sy + 96.0;
    const float tex = 0.78 + 0.22 * Hash(floor((p - float2(sx, sy)) / 4.0));
    const float v = inside ? tex : bg;
    o[id.xy] = float4(v, v, v, 1.0) * scale;
}
)";

struct Shown { LARGE_INTEGER at; bool made; ID3D11Texture2D* staging; };
std::vector<Shown> g_shown;
ID3D11Device* g_dev = nullptr; ID3D11DeviceContext* g_ctx = nullptr;
UINT g_w = 0, g_h = 0; DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
uint32_t g_encoding = 0; float g_white = 80.0f;

double g_t0 = 0;   // the time of the first log line, for the lines that follow
void Log(const char* m) {
    LARGE_INTEGER n, f; QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f);
    const double ms = n.QuadPart * 1000.0 / f.QuadPart; if (g_t0 == 0) g_t0 = ms;
    printf("[presenter %6.0f ms] %s\n", ms - g_t0, m); fflush(stdout);
}

LARGE_INTEGER g_lastEntry{}; double g_maxGapMs = 0; int g_entries = 0;   // how far apart the presenting thread's calls into the presenter are: a stall of the thread shows as a long gap
void OnHook(IDXGISwapChain* sc, UINT sync, UINT flags) {
    { LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
      if (g_lastEntry.QuadPart && g_entries > 2) g_maxGapMs = std::max(g_maxGapMs, (now.QuadPart - g_lastEntry.QuadPart) * 1000.0 / f.QuadPart);
      g_lastEntry = now; ++g_entries; }
    nr::framegen::BeforeRealPresent(sc, sync, flags, g_encoding, g_white, Log, [](ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, bool made) {
        if (g_shown.size() >= 600) return;
        D3D11_TEXTURE2D_DESC d{}; frame->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        ID3D11Texture2D* st = nullptr;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) return;
        ctx->CopyResource(st, frame);
        Shown s{}; QueryPerformanceCounter(&s.at); s.made = made; s.staging = st; g_shown.push_back(s);
    });
}

// the square's left edge and centre x in a frame (bright pixels in the rows the square covers), or -1
double SquareX(ID3D11Texture2D* st) {
    D3D11_MAPPED_SUBRESOURCE m{};
    D3D11_TEXTURE2D_DESC sd{}; st->GetDesc(&sd);
    if (FAILED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) return -1;
    double sum = 0; uint64_t n = 0;
    for (UINT y = 330; y < 400 && y < sd.Height; y += 2) {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        for (UINT x = 0; x < sd.Width; ++x) {
            float v;
            if (g_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) v = DirectX::PackedVector::XMConvertHalfToFloat(reinterpret_cast<const uint16_t*>(row)[x * 4]);
            else v = row[x * 4] / 255.0f;
            if (v > 0.68f) { sum += x; ++n; }
        }
    }
    g_ctx->Unmap(st, 0);
    return n > 50 ? sum / static_cast<double>(n) : -1;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int mult = std::clamp(Arg(argc, argv, "mult", 4), 2, 9), frames = std::clamp(Arg(argc, argv, "frames", 30), 8, 400);
    const bool doToggle = Arg(argc, argv, "toggle", 0) != 0;
    const bool hdr = Arg(argc, argv, "hdr", 0) != 0, own = doToggle ? false : Arg(argc, argv, "own", 1) != 0;
    // (the square must stay on the 960 pixel wide picture: 120 + speed * frames + 96 < 960)
    const int fps = std::clamp(Arg(argc, argv, "fps", 30), 10, 120); const double speed = Arg(argc, argv, "speed", 24);
    const int dup = std::clamp(Arg(argc, argv, "dup", 1), 1, 4); const bool doSwitch = Arg(argc, argv, "switch", 0) != 0, doResize = Arg(argc, argv, "resize", 0) != 0;
    int wantMult = mult, settledFrom = 0;   // the multiplier the last intervals should have, and the first real frame from which the checks look
    const int sizeX = std::clamp(Arg(argc, argv, "size", 1), 1, 4), syncInt = std::clamp(Arg(argc, argv, "sync", 0), 0, 2), latencyN = std::clamp(Arg(argc, argv, "latency", 0), 0, 8), buffersN = std::clamp(Arg(argc, argv, "buffers", 3), 2, 6);
    const bool tearing = Arg(argc, argv, "tearing", 0) != 0;
    g_w = 960 * sizeX; g_h = 540 * sizeX; g_fmt = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    g_encoding = hdr ? 1u : 0u; g_white = 80.0f;
    std::wstring fsr;
    { const std::string a = ArgText(argc, argv, "fsr"); if (!a.empty()) fsr = std::wstring(a.begin(), a.end());
      else { wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH); fsr = exe; fsr = fsr.substr(0, fsr.find_last_of(L'\\')) + L"\\fsr\\amd_fidelityfx_dx12.dll"; } }

    WNDCLASSEXW wc{ sizeof wc }; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"NrFgPresent"; RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"nr fgpresent", WS_POPUP | WS_VISIBLE, 0, 0, g_w, g_h, nullptr, nullptr, wc.hInstance, nullptr);
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx))) { printf("no device\n"); return 2; }
    IDXGIDevice* dxgi = nullptr; IDXGIAdapter* adapter = nullptr; IDXGIFactory2* factory = nullptr;
    g_dev->QueryInterface(IID_PPV_ARGS(&dxgi)); dxgi->GetAdapter(&adapter); adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = g_w; sd.Height = g_h; sd.Format = g_fmt; sd.SampleDesc.Count = 1; sd.BufferCount = buffersN;
    sd.Flags = (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u) | (latencyN ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0u);
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    if (FAILED(factory->CreateSwapChainForHwnd(g_dev, hwnd, &sd, nullptr, nullptr, &sc1))) { printf("no swap chain\n"); return 2; }
    IDXGISwapChain* sc = sc1;
    HANDLE waitable = nullptr;
    if (latencyN) { IDXGISwapChain2* sc2 = nullptr; if (SUCCEEDED(sc1->QueryInterface(IID_PPV_ARGS(&sc2)))) { sc2->SetMaximumFrameLatency(static_cast<UINT>(latencyN)); waitable = sc2->GetFrameLatencyWaitableObject(); sc2->Release(); } }
    const UINT presentFlags = (tearing && syncInt == 0) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    std::vector<double> realPresentMs;   // how long the real frame's Present call took (the presenter's work and waits are in it)

    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(kScene, strlen(kScene), "scene", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) { printf("scene shader: %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); return 2; }
    ID3D11ComputeShader* cs = nullptr; g_dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs); code->Release();
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer* cb = nullptr; g_dev->CreateBuffer(&bd, nullptr, &cb);

    if (!PresentHook::Install(g_dev, OnHook, Log)) { printf("the Present hook could not be installed\n"); return 2; }
    nr::framegen::SetRuntime(fsr); nr::framegen::SetOwn(own); nr::framegen::SetMultiplier(mult); nr::framegen::SetAlign(Arg(argc, argv, "align", 1) != 0);

    const double intervalMs = 1000.0 / fps, x0 = 120.0;
    LARGE_INTEGER freq, start; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start);
    auto nowMs = [&] { LARGE_INTEGER n; QueryPerformanceCounter(&n); return (n.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart; };
    auto drawFrame = [&](double x) {
        if (waitable) WaitForSingleObjectEx(waitable, 200, TRUE);
        ID3D11Texture2D* bb = nullptr; sc->GetBuffer(0, IID_PPV_ARGS(&bb));
        ID3D11UnorderedAccessView* uav = nullptr; g_dev->CreateUnorderedAccessView(bb, nullptr, &uav);
        const float c[4] = { static_cast<float>(x), 330.0f, 1.0f, 0.0f };
        g_ctx->UpdateSubresource(cb, 0, nullptr, c, 0, 0);
        g_ctx->CSSetShader(cs, nullptr, 0); g_ctx->CSSetConstantBuffers(0, 1, &cb); g_ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        g_ctx->Dispatch((g_w + 7) / 8, (g_h + 7) / 8, 1);
        ID3D11UnorderedAccessView* none = nullptr; g_ctx->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
        uav->Release(); bb->Release();
        LARGE_INTEGER p0, p1; QueryPerformanceCounter(&p0);
        sc->Present(static_cast<UINT>(syncInt), presentFlags);
        QueryPerformanceCounter(&p1); realPresentMs.push_back((p1.QuadPart - p0.QuadPart) * 1000.0 / freq.QuadPart);
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    };
    {   // warm-up: the picture still, until the engine (and EchoFG, when wanted) is up; the presenting thread must not wait for it
        const char* const wantEngine = own ? "EchoFG" : "FSR";
        int ready = 0; double warmStart = nowMs();
        for (int n = 0; n < 600 && ready < 3; ++n) {
            while (nowMs() < warmStart + n * intervalMs) Sleep(0);
            drawFrame(x0);
            if (nr::framegen::GetStats().engine.rfind(wantEngine, 0) == 0) ++ready;
        }
        printf("warm-up: the engine was up after %.0f ms\n", nowMs() - warmStart);
        for (auto& s : g_shown) if (s.staging) s.staging->Release();
        g_shown.clear();
        QueryPerformanceCounter(&start);
    }
    for (int n = 0; n < frames; ++n) {
        if (doToggle && n == 10) nr::framegen::SetOwn(true);
        if (doSwitch && n == frames / 3) nr::framegen::SetMultiplier(2);
        if (doSwitch && n == 2 * frames / 3) { nr::framegen::SetMultiplier(5); wantMult = 5; settledFrom = n + 3; }
        if (doResize && n == 20) {
            if (FAILED(sc->ResizeBuffers(0, 800, 450, DXGI_FORMAT_UNKNOWN, 0))) { printf("the resize failed\n"); return 2; }
            g_w = 800; g_h = 450; settledFrom = n + 4;
        }
        while (nowMs() < n * intervalMs) Sleep(0);
        drawFrame(x0 + speed * (n / dup));
    }
    g_ctx->Flush();
    Sleep(300);

    // what was shown, in order: a real frame (made = false) ends each interval; the made ones come before it
    const nr::framegen::Stats st = nr::framegen::GetStats();
    printf("presenter: %llu real, %llu made between (%llu by %s), %zu frames shown\n", (unsigned long long)st.real, (unsigned long long)st.generated, (unsigned long long)st.byFsr, st.engine.c_str(), g_shown.size());
    if (Arg(argc, argv, "list", 0)) { double t0 = 0; for (size_t i = 0; i < g_shown.size() && i < 400; ++i) { if (!i) t0 = static_cast<double>(g_shown[0].at.QuadPart); printf("  shown %2zu  %s  x %7.1f  at %7.1f ms\n", i, g_shown[i].made ? "made" : "real", SquareX(g_shown[i].staging), (g_shown[i].at.QuadPart - t0) * 1000.0 / freq.QuadPart); } }
    int made = 0, real = 0, bad = 0, orderBad = 0, interval = 0, inInterval = 0; double worst = 0, sumErr = 0; int measured = 0;
    std::vector<double> gaps; LARGE_INTEGER last{}; int countWrong = 0, warm = std::max(6, settledFrom);
    std::vector<int> perInterval; bool started = false;   // (the engine's first frames take a while: the first shaders compile; checks start at the first interval with the right count)
    for (size_t i = 0; i < g_shown.size(); ++i) {
        const Shown& s = g_shown[i];
        const double x = SquareX(s.staging);
        if (last.QuadPart && static_cast<int>(real) > warm) gaps.push_back((s.at.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart);
        last = s.at;
        if (s.made) { ++made; ++inInterval; }
        else {
            // a real frame n: x = x0 + speed n; the made frames before it, k = 1 .. inInterval of inInterval + 1
            if (real > 0 && real > warm && !started) { if (inInterval == wantMult - 1) started = true; }
            else if (real > 0 && real > warm) {
                perInterval.push_back(inInterval);
                if (inInterval != wantMult - 1) ++countWrong;
                for (int k = 1; k <= inInterval; ++k) {
                    const Shown& m = g_shown[i - inInterval + (k - 1)];
                    const double cPrev = (real - 1) / dup, cNow = real / dup;   // (the content index of the real frames each side)
                    const double mx = SquareX(m.staging), want = x0 + speed * (cPrev + (cNow - cPrev) * static_cast<double>(k) / (inInterval + 1));
                    const double base = x0 + speed * cNow;   // this real frame's measured x is base + offset (the centroid sits half the square in)
                    const double offset = x - base;
                    const double e = std::fabs(mx - (want + offset));
                    if (mx < 0) { ++bad; continue; }
                    worst = std::max(worst, e); sumErr += e; ++measured;
                }
            }
            ++real; inInterval = 0; ++interval;
        }
    }
    (void)orderBad;
    double minGap = 1e9, maxGap = 0, meanGap = 0;
    for (double g : gaps) { minGap = std::min(minGap, g); maxGap = std::max(maxGap, g); meanGap += g; }
    if (!gaps.empty()) meanGap /= gaps.size();
    const double want = intervalMs / wantMult;
    printf("the longest wait between the presenting thread's calls into the presenter: %.0f ms (a real frame every %.0f ms)\n", g_maxGapMs, intervalMs);
    const bool stalled = g_maxGapMs > std::max(150.0, 4.0 * intervalMs);   // (the engine's very first frame takes about 50 ms once; the start-up used to take seconds)
    const bool toggled = !doToggle || st.engine.rfind("EchoFG", 0) == 0;
    if (doToggle) printf("EchoFG after the switch: %s (the engine says %s)\n", toggled ? "running" : "NOT running", st.engine.c_str());
    printf("frames per real frame after warm-up: %s (wanted %d made between each two real ones; %d intervals wrong)\n", perInterval.empty() ? "none" : "measured", wantMult - 1, countWrong);
    printf("the made frames' square position: mean error %.2f px, worst %.2f px (%d frames; speed %.0f px a real frame); %d frames without a readable square\n", measured ? sumErr / measured : 0.0, worst, measured, speed, bad);
    printf("spacing of the frames shown: mean %.1f ms, min %.1f, max %.1f (wanted %.1f ms at x%d of %d fps)\n", meanGap, minGap, maxGap, want, wantMult, fps);
    {   // the study: how long the Present calls took
        std::vector<double> rp(realPresentMs.begin() + std::min<size_t>(realPresentMs.size(), 8 + 2 * static_cast<size_t>(std::max(0, settledFrom))), realPresentMs.end());   // (after the warm-up frames and the engine's start)
        std::sort(rp.begin(), rp.end());
        if (!rp.empty()) printf("Present of the real frame (with the presenter's work and waits): median %.1f ms, 90%% %.1f, worst %.1f; of each frame between: %.1f ms (smoothed)   [sync=%d tearing=%d latency=%d buffers=%d size=%d]\n",
            rp[rp.size() / 2], rp[std::min(rp.size() - 1, rp.size() * 9 / 10)], rp.back(), st.generatedMs, syncInt, tearing ? 1 : 0, latencyN, buffersN, sizeX);
        DXGI_FRAME_STATISTICS fs{}; if (SUCCEEDED(sc->GetFrameStatistics(&fs))) printf("display: %u presents counted, %u refreshes\n", fs.PresentCount, fs.PresentRefreshCount);
    }
    if (stalled) printf("the presenting thread stalled\n");
    const bool ok = made > 0 && countWrong == 0 && bad == 0 && measured > 0 && worst < 0.2 * speed + 3.0 && !stalled && toggled;
    printf("%s\n", ok ? "FG PRESENT TEST PASSED" : "FG PRESENT TEST FAILED");
    for (auto& s : g_shown) if (s.staging) s.staging->Release();
    nr::framegen::Shutdown(); PresentHook::Uninstall();
    return ok ? 0 : 1;
}
