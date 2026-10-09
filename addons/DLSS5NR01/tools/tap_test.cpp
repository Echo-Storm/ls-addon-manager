// Offline test of FrameTap (src/addon/frame_tap.*): how Neural Rendering reads Lossless Scaling's compute passes. A real D3D11 device and textures
// shaped like LSFG 3's are bound the way Lossless Scaling binds them, and FrameTap::Observe is called for each pass as the Dispatch hook would.
// Nothing is dispatched, so any GPU (or WARP) will do, and no model or NVIDIA SDK is needed.
//   nr_taptest.exe
// It pins down: which pass is the capture (TAP), the frame handed over and its slot, this frame's own motion (the flow written after the TAP) with the
// hand-over one dispatch after the finest flow pass, the old timing when fresh flow is off, a frame dropped when LSFG runs no flow pass, a change of flow
// size, where each present sits between real frames, and that every texture it hands out or keeps is released.
#include "addon/frame_tap.h"
#include <d3d11.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#pragma comment(lib, "d3d11.lib")

static int g_failed = 0;
static void Check(const char* what, bool ok, const std::string& detail = "") {
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  (", detail.empty() ? "" : (detail + ")").c_str());
    if (!ok) ++g_failed;
}

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;

struct Tex {
    ID3D11Texture2D* t = nullptr; ID3D11ShaderResourceView* srv = nullptr; ID3D11UnorderedAccessView* uav = nullptr;
    void Make(UINT w, UINT h, DXGI_FORMAT f) {
        D3D11_TEXTURE2D_DESC d = {}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        g_dev->CreateTexture2D(&d, nullptr, &t); g_dev->CreateShaderResourceView(t, nullptr, &srv); g_dev->CreateUnorderedAccessView(t, nullptr, &uav);
    }
    void Free() { if (uav) uav->Release(); if (srv) srv->Release(); if (t) t->Release(); t = nullptr; srv = nullptr; uav = nullptr; }
    ULONG Refs() { t->AddRef(); return t->Release(); }
};

static long long g_observeTicks = 0, g_observeCalls = 0;   // the time spent inside FrameTap::Observe (bench=1)
static void Unbind() {
    ID3D11ShaderResourceView* s[8] = {}; ID3D11UnorderedAccessView* u[4] = {};
    g_ctx->CSSetShaderResources(0, 8, s); g_ctx->CSSetUnorderedAccessViews(0, 4, u, nullptr);
}
// One pass: bind the views, let FrameTap observe it, unbind. Returns whether FrameTap hands a frame over at this pass.
static bool Pass(FrameTap& tap, std::vector<std::pair<int, Tex*>> srvs, std::vector<std::pair<int, Tex*>> uavs, UINT x, UINT y, TapDecision& d) {
    for (auto& s : srvs) g_ctx->CSSetShaderResources(s.first, 1, &s.second->srv);
    for (auto& u : uavs) g_ctx->CSSetUnorderedAccessViews(u.first, 1, &u.second->uav, nullptr);
    LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
    const bool run = tap.Observe(g_ctx, x, y, 1, d);
    QueryPerformanceCounter(&q1); g_observeTicks += q1.QuadPart - q0.QuadPart; ++g_observeCalls;
    Unbind();
    return run;
}

// LSFG-like textures for a 1280x720 capture: the frame, a luma pyramid, two flow chains that alternate per real frame (so "this frame's flow" and the
// previous frame's are different textures), and the generated frame
struct Lsfg {
    Tex frame[2], pyr0, pyr1, flowCoarse[2], flowFine[2], flowFineSmall[2], gen;
    void Make() {
        for (int i = 0; i < 2; ++i) { frame[i].Make(1280, 720, DXGI_FORMAT_B8G8R8A8_UNORM); flowCoarse[i].Make(160, 90, DXGI_FORMAT_R16G16B16A16_FLOAT);
                                      flowFine[i].Make(320, 180, DXGI_FORMAT_R16G16B16A16_FLOAT); flowFineSmall[i].Make(240, 135, DXGI_FORMAT_R16G16B16A16_FLOAT); }
        pyr0.Make(640, 360, DXGI_FORMAT_R8_UNORM); pyr1.Make(320, 180, DXGI_FORMAT_R8_UNORM);
        gen.Make(1280, 720, DXGI_FORMAT_B8G8R8A8_UNORM);
    }
    void Free() { for (int i = 0; i < 2; ++i) { frame[i].Free(); flowCoarse[i].Free(); flowFine[i].Free(); flowFineSmall[i].Free(); } pyr0.Free(); pyr1.Free(); gen.Free(); }
};

struct FrameResult {
    int handOvers = 0;                 // passes at which a frame was handed over
    int atPass = -1;                   // which pass (0 = the TAP) handed it over
    ID3D11Texture2D* frame = nullptr; int slot = -1;
    ID3D11Texture2D* flow = nullptr; bool fresh = false;
};

// One real frame k as LSFG 3 runs it: the capture pass (TAP), flow passes coarse -> fine (main, then auxiliary at the finest level), a generated frame,
// and small work in between. `flowPass` false: LSFG skips the flow (a duplicated capture). `coarser` true: a coarser finest level (a lower flow scale).
static FrameResult RunFrame(FrameTap& tap, Lsfg& l, int k, bool flowPass = true, bool coarser = false) {
    FrameResult r; int pass = 0;
    Tex& cur = l.frame[k & 1]; Tex& prev = l.frame[(k + 1) & 1];
    Tex& coarse = l.flowCoarse[k & 1]; Tex& fine = coarser ? l.flowFineSmall[k & 1] : l.flowFine[k & 1];
    auto note = [&](bool run, TapDecision& d) {
        if (run) { ++r.handOvers; r.atPass = pass; r.frame = d.frame; r.slot = d.frameSlot; r.flow = d.flow; r.fresh = d.freshFlow;
                   if (d.frame) d.frame->Release(); if (d.flow) d.flow->Release(); }
        else { if (d.frame) d.frame->Release(); if (d.flow) d.flow->Release(); }
        ++pass;
    };
    TapDecision d;
    note(Pass(tap, { { 0, &prev }, { 1, &cur } }, { { 0, &l.pyr0 }, { 1, &l.pyr1 } }, 160, 90, d), d);                      // TAP: both frames in, pyramid (R8) out
    note(Pass(tap, { { 0, &l.pyr1 } }, { { 0, &l.pyr1 } }, 40, 23, d), d);                                                // small work
    if (flowPass) {
        note(Pass(tap, { { 4, &l.pyr1 } }, { { 0, &coarse } }, 20, 12, d), d);                                            // flow, coarse level (coarser level at S4)
        note(Pass(tap, { { 4, &coarse } }, { { 0, &fine } }, 40, 23, d), d);                                              // flow, finest level: this frame's main flow
        note(Pass(tap, { { 4, &coarse } }, { { 0, &fine } }, 40, 23, d), d);                                              // the auxiliary field at the same level
    }
    note(Pass(tap, { { 0, &prev }, { 1, &cur }, { 2, &fine } }, { { 0, &l.gen } }, 160, 90, d), d);                          // a generated frame
    return r;
}

int main(int argc, char** argv) {
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_dev, &fl, &g_ctx))) { printf("FAIL  no D3D11 device\n"); return 2; }
    Lsfg l; l.Make();
    const ULONG baseFrame = l.frame[0].Refs(), baseFlow = l.flowFine[0].Refs();

    {
        printf("== finding the capture pass, and this frame's own motion (the default)\n");
        FrameTap tap;
        std::vector<FrameResult> r;
        for (int k = 0; k < 40; ++k) r.push_back(RunFrame(tap, l, k));
        Check("one real frame per TAP", tap.Taps() == 40, std::to_string(tap.Taps()));
        int onePerFrame = 0; for (int k = 2; k < 40; ++k) if (r[k].handOvers == 1) ++onePerFrame;
        Check("from the third frame on, exactly one hand-over per real frame", onePerFrame == 38, std::to_string(onePerFrame));
        bool frameOk = true, flowOk = true, whenOk = true, freshOk = true;
        for (int k = 2; k < 40; ++k) {
            frameOk &= r[k].frame == l.frame[k & 1].t && r[k].slot == 1;
            flowOk &= r[k].flow == l.flowFine[k & 1].t;            // written after this frame's TAP
            whenOk &= r[k].atPass == 4;                            // the dispatch after the first finest-level flow pass
            freshOk &= r[k].fresh;
        }
        Check("the frame handed over is this frame's capture, from the highest slot holding it", frameOk);
        Check("its motion is this frame's own flow (the one written after the TAP), not the previous frame's", flowOk);
        Check("it is handed over on the dispatch right after the finest flow pass", whenOk, std::to_string(r[10].atPass));
        Check("...and marked as fresh", freshOk);
        Check("the counters say so", tap.FreshRuns() == 38 && tap.StaleRuns() == 2 && tap.DroppedWaiting() == 0, std::to_string(tap.FreshRuns()) + " fresh, " + std::to_string(tap.StaleRuns()) + " previous, " + std::to_string(tap.DroppedWaiting()) + " dropped");
        Check("the first frames (flow size not known yet) are handed over at the TAP with the previous flow", r[0].atPass == 0 && r[1].atPass == 0 && !r[1].fresh && tap.StaleRuns() >= 2);

        printf("== LSFG runs no flow pass for a frame\n");
        const uint64_t droppedBefore = tap.DroppedWaiting(), freshBefore = tap.FreshRuns();
        const FrameResult skipped = RunFrame(tap, l, 40, false);
        const FrameResult next = RunFrame(tap, l, 41);
        Check("that frame is not handed over late", skipped.handOvers == 0);
        Check("...it counts as dropped when the next frame arrives", tap.DroppedWaiting() == droppedBefore + 1);
        Check("...and the next frame gets its own motion as usual", next.handOvers == 1 && next.fresh && next.flow == l.flowFine[41 & 1].t && tap.FreshRuns() == freshBefore + 1);

        printf("== the flow gets coarser (a lower flow scale in Lossless Scaling)\n");
        int lost = 0, freshAfter = 0;
        for (int k = 42; k < 50; ++k) { const FrameResult f = RunFrame(tap, l, k, true, true); if (f.handOvers == 0) ++lost; else if (f.fresh && f.flow == l.flowFineSmall[k & 1].t) ++freshAfter; }
        Check("at most one frame is lost while the new size is learned, then every frame gets its own motion again", lost <= 1 && freshAfter >= 7, std::to_string(lost) + " lost, " + std::to_string(freshAfter) + " fresh");

        printf("== frame generation switched off: the capture goes on, the flow passes stop\n");
        int lostOff = 0, atTapOff = 0, withFlowOff = 0;
        for (int k = 50; k < 62; ++k) {
            const FrameResult f = RunFrame(tap, l, k, false);
            if (f.handOvers == 0) ++lostOff; else if (f.atPass == 0) ++atTapOff;
            if (f.flow) ++withFlowOff;
        }
        Check("after a few frames without flow, frames are handed over at the TAP instead of all being dropped", lostOff <= 3 && atTapOff >= 9,
              std::to_string(lostOff) + " lost, " + std::to_string(atTapOff) + " at the TAP");
        Check("...with no motion rather than the last flow from before", withFlowOff == 0, std::to_string(withFlowOff) + " with a flow");
        int freshBack = 0;
        for (int k = 62; k < 68; ++k) { const FrameResult f = RunFrame(tap, l, k); if (f.handOvers == 1 && f.fresh && f.flow == l.flowFine[k & 1].t) ++freshBack; }
        Check("switched back on, every frame gets its own motion again (at most one frame lost)", freshBack >= 5, std::to_string(freshBack) + " of 6 fresh");

        printf("== presents between real frames\n");
        // X2, real frame last: generated, real per real frame
        tap.Reset();
        for (int k = 0; k < 4; ++k) {
            RunFrame(tap, l, k);
            tap.NotePresent();
            tap.NotePresent();
        }
        RunFrame(tap, l, 4);
        PresentInfo a = tap.NotePresent(), b = tap.NotePresent();
        Check("two presents per real frame are learned", b.perFrame == 2);
        Check("a present with a generated frame composed before it is marked generated", a.gen && !b.gen);
        Check("the generated one sits half way between the previous real frame and this one, the real one on this one", a.target > a.tap - 0.51 && a.target < a.tap - 0.49 && b.target > b.tap - 0.01 && b.target < b.tap + 0.01,
              std::to_string(a.target) + " / " + std::to_string(b.target));
        uint32_t w = 0, h = 0;
        ID3D11Resource* nf = tap.NewestFlow(w, h);
        Check("the newest flow is the one written for the latest frame", nf == l.flowFine[4 & 1].t && w == 320 && h == 180);
        if (nf) nf->Release();
    }

    {
        printf("== fresh flow off: the old timing\n");
        FrameTap tap;
        tap.SetFreshFlow(false);
        std::vector<FrameResult> r;
        for (int k = 0; k < 12; ++k) r.push_back(RunFrame(tap, l, k));
        bool atTap = true, prevFlow = true;
        for (int k = 3; k < 12; ++k) { atTap &= r[k].atPass == 0 && r[k].handOvers == 1; prevFlow &= r[k].flow == l.flowFine[(k - 1) & 1].t && !r[k].fresh; }
        Check("every frame is handed over at its TAP", atTap);
        Check("...with the previous frame's flow", prevFlow);
        Check("the counters say so", tap.FreshRuns() == 0 && tap.StaleRuns() == 12, std::to_string(tap.StaleRuns()));
    }

    {
        printf("== nothing held after a reset or when switched off\n");
        FrameTap tap;
        for (int k = 0; k < 5; ++k) RunFrame(tap, l, k);
        TapDecision d;   // a TAP with no flow pass after it: the frame is held
        Pass(tap, { { 0, &l.frame[1] }, { 1, &l.frame[0] } }, { { 0, &l.pyr0 }, { 1, &l.pyr1 } }, 160, 90, d);
        if (d.frame) d.frame->Release(); if (d.flow) d.flow->Release();
        tap.Reset();
        tap.SetFreshFlow(false);
    }
    if (argc > 1 && !strcmp(argv[1], "bench")) {   // tap_test bench: what one dispatch costs the addon on Lossless Scaling's render thread (CPU), per loaded addon
        LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
        for (int withFlow = 1; withFlow >= 0; --withFlow) {
            FrameTap tap; for (int k = 0; k < 30; ++k) RunFrame(tap, l, k, withFlow != 0);   // (learn the roles first)
            g_observeTicks = g_observeCalls = 0;
            for (int k = 0; k < 4000; ++k) RunFrame(tap, l, k + 30, withFlow != 0);
            printf("bench: %s: %.2f us per dispatch inside Observe (%lld dispatches); a frame of %d dispatches costs %.1f us of CPU per loaded addon\n", withFlow ? "frame generation on" : "no flow passes (frame generation off)",
                   1e6 * g_observeTicks / freq.QuadPart / g_observeCalls, g_observeCalls, withFlow ? 5 : 3, 1e6 * g_observeTicks / freq.QuadPart / 4000);
        }
    }
    const ULONG f0 = l.frame[0].Refs(), f1 = l.frame[1].Refs(), w0 = l.flowFine[0].Refs(), w1 = l.flowFine[1].Refs(), s0 = l.flowFineSmall[0].Refs(), s1 = l.flowFineSmall[1].Refs();
    Check("every texture handed out or held was released, also when a FrameTap is destroyed (no reference left behind)",
          f0 == baseFrame && f1 == baseFrame && w0 == baseFlow && w1 == baseFlow && s0 == baseFlow && s1 == baseFlow,
          "frames " + std::to_string(f0) + "," + std::to_string(f1) + " flows " + std::to_string(w0) + "," + std::to_string(w1) + "," + std::to_string(s0) + "," + std::to_string(s1) +
          " (each should be " + std::to_string(baseFrame) + ")");

    l.Free(); g_ctx->Release(); g_dev->Release();
    printf("\n%s (%d failed)\n", g_failed ? "TAP TEST FAILED" : "TAP TEST PASSED", g_failed);
    return g_failed ? 1 : 0;
}
