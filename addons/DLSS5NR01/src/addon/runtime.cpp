// The frame path, on Lossless Scaling's render thread and its graphics card.
//
//   capture k ─┬─ LSFG's capture pass on frame k       <- the TAP (with fresh motion: just after the frame's flow pass): frame k and LSFG's flow
//              │                                          are copied to the model's input and a run starts, unless the model is still busy. The
//              │                                          frame itself is only read.
//              ├─ LSFG's flow and generated frames ...
//              └─ Present (generated, generated, real)  <- each present: the newest finished delta is added to the frame about to be shown,
//                                                          moved along LSFG's flow to where that content is in this frame.
//
// Lossless Scaling's queue never waits for the model: the model can be late for a frame, never slow Lossless Scaling down.
#include "addon/state.h"
#include "addon/log.h"
#include "addon/frame_trace.h"
#include "addon/diagnosis.h"
#include "addon/vram.h"
#include "addon/present_hook.h"
#include "addon/framegen11.h"
#include "addon/screenshot.h"
#include "addon/scaler11.h"
#include "addon/compare.h"
#include "addon/hdr.h"
#include "engine/sr_engine.h"
#include "engine/ngx_paths.h"
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "version.lib")
#include <d3d11_1.h>
#include <dxgi.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

namespace nr {

namespace {

bool SameCard(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

std::string Utf8(const std::wstring& w) {
    std::string s(w.size() * 3, '\0');
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    s.resize(n > 0 ? n : 0);
    return s;
}

const char* FormatName(uint32_t f) {
    switch (f) {
    case 87: return "BGRA8"; case 91: return "BGRA8s"; case 28: return "RGBA8"; case 29: return "RGBA8s"; case 24: return "RGB10A2";
    case 10: return "RGBA16F"; case 34: return "RG16F"; case 16: return "RG32F"; case 49: return "RG8"; case 41: return "R32F";
    case 54: return "R16F"; case 61: return "R8"; case 26: return "R11G11B10F"; case 2: return "RGBA32F"; default: return "?";
    }
}

// The card a device is on, and whether it is NVIDIA's.
struct Card { LUID luid{}; std::string name = "?"; bool nvidia = false; bool drivesDisplay = false; };
Card CardOf(ID3D11Device* dev) {
    Card card;
    IDXGIDevice* dxgi = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgi)))) return card;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(dxgi->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC desc; adapter->GetDesc(&desc);
        card.luid = desc.AdapterLuid; card.name = Utf8(desc.Description); card.nvidia = desc.VendorId == 0x10DE;
        IDXGIOutput* output = nullptr;
        card.drivesDisplay = SUCCEEDED(adapter->EnumOutputs(0, &output));
        if (output) output->Release();
        adapter->Release();
    }
    dxgi->Release();
    return card;
}

// ---- the render thread's own state (under g_frameMutex)

struct SeenDevice { ID3D11Device* dev; bool nvidia; LUID card; };
std::vector<SeenDevice> g_seen;               // each device that has dispatched, judged once (not AddRef'd; forgotten at device events)
ID3D11Device* g_tapDevice = nullptr;          // the device the bridge works with
LUID g_candidateCard{}; uint32_t g_candidateTaps = 0;   // frames arriving from a card the engine is not on
constexpr uint32_t kFollowAfterTaps = 20;
LUID g_failedCard{}; bool g_failedCardKnown = false;     // the card the engine last failed on: it is not tried again by itself
IDXGISwapChain* g_lsChain = nullptr;          // Lossless Scaling's swap chain
IDXGISwapChain* g_otherChain = nullptr;       // the last one seen on another device (the manager's window presents between Lossless Scaling's)
uint64_t g_presents = 0;
uint64_t g_presentStages[5] = {};             // diagnostics: how far presents get
uint32_t g_watchdogHits = 0;
uint64_t g_lastRunAtMs = 0;                   // when the model last started a run: an older result is not composed (see Compose)
constexpr uint64_t kMaxResultAgeMs = 500;
thread_local bool t_ownWork = false;          // our own compose pass runs on Lossless Scaling's context: its dispatch is not a pass of theirs

std::atomic<uint32_t> g_marker{ 0 };          // the corner square after a hotkey, and until when
std::atomic<uint64_t> g_markerUntil{ 0 };
void ShowMarker(uint32_t colour) { g_marker = colour; g_markerUntil = GetTickCount64() + 1200; }

void ReleaseDecision(TapDecision& d) {
    if (d.frame) d.frame->Release();
    if (d.flow) d.flow->Release();
    d.frame = nullptr; d.flow = nullptr;
}

bool g_presentMode = false;   // frame generation off: the model takes the presented frame (see PresentTap)
bool g_presentWait = false;   // ...and each compose waits for its own frame's result (Config::presentWait)
uint64_t g_tapsSeen = 0, g_presentIndex = 0, g_presentOwn = 0, g_presentEarlier = 0, g_tapSeenAtMs = 0;
int g_presentsWithoutTap = 0;
ID3D11Device* g_presentHookTriedOn = nullptr;   // the device the Present hook was last tried from (once per device, not every pass)
void ForgetPair();

void ForgetDevice() {   // under g_frameMutex
    screenshot::Forget();
    ForgetPair();
    g_recorder.Forget();
    g_bridge.Shutdown(); g_compose.Shutdown(); g_tap.Reset();
    g_presentMode = false; g_tapsSeen = 0; g_presentsWithoutTap = 0; g_presentHookTriedOn = nullptr;
    g_seen.clear(); g_tapDevice = nullptr; g_lsChain = nullptr; g_otherChain = nullptr;
}

// ---- live numbers for the manager (its Performance tab and the addon's card)

void PublishLive(const NrStats& st) {
    static uint64_t slowAt = 0, statusAt = 0, runsBefore = 0, skippedBefore = 0;
    static double keepUp = 100.0;
    const double interval = g_bridge.LastIntervalMs();
    if (interval > 0) g_host->PublishMetric(kAddonId, "frame_ms", interval, "ms");
    const uint64_t now = GetTickCount64();
    if (now - slowAt >= 200) {   // five times a second is plenty for the rest
        slowAt = now;
        const uint64_t runs = g_bridge.Runs(), skipped = g_bridge.Skipped();
        if (runs < runsBefore || skipped < skippedBefore) runsBefore = skippedBefore = 0;   // the bridge started again (its counts begin at 0): not a huge unsigned difference
        const uint64_t newRuns = runs - runsBefore, newSkipped = skipped - skippedBefore;
        runsBefore = runs; skippedBefore = skipped;
        if (newRuns + newSkipped) keepUp = 100.0 * newRuns / static_cast<double>(newRuns + newSkipped);
        g_host->PublishMetric(kAddonId, "model_ms", st.nrMs, "ms");
        g_host->PublishMetric(kAddonId, "model_total_ms", st.totalMs, "ms");
        { std::lock_guard<std::mutex> lock(g_autoMutex); if (g_auto.Scale() > 0) g_host->PublishMetric(kAddonId, "model_scale", g_auto.Scale(), "x"); }
        g_host->PublishMetric(kAddonId, "gpu_start_ms", st.startMs, "ms");
        g_host->PublishMetric(kAddonId, "keepup_pct", keepUp, "%");
        g_host->PublishMetric(kAddonId, "tap_cpu_ms", g_bridge.CpuMs(), "ms");
    }
    if (now - statusAt >= 1000) {
        statusAt = now;
        char text[96];
        snprintf(text, sizeof text, keepUp >= 90.0 ? "Running, model %.1f ms, keeps up %.0f%%" : "Model is behind: %.1f ms, keeps up %.0f%%", st.nrMs, keepUp);
        g_host->SetStatus(kAddonId, text, keepUp >= 90.0 ? 1 : 2);
    }
}

// The panel's "What is wrong" card: the findings for the game's frame-time spread over the last window and what the model or the upscaler did meanwhile.
void UpdateFindings(int frames, float p50, float p95, float p99, const NrStats* st, float upscalerMs = 0, float motionMs = 0) {
    nr::diag::Snapshot d;
    d.frames = frames; d.frameP50 = p50; d.frameP95 = p95; d.frameP99 = p99;
    d.showingPlain = g_compare.load() == 2;
    bool autoOn; float floor;
    { std::lock_guard<std::mutex> lock(g_settingsMutex); autoOn = g_config.autoQuality; floor = g_config.autoFloor; d.recording = g_config.recordOn; d.recordingShown = g_config.recordShown; }
    if (st) {
        static uint64_t runsBefore = 0, skippedBefore = 0;
        const uint64_t runs = g_bridge.Runs(), skipped = g_bridge.Skipped();
        if (runs < runsBefore || skipped < skippedBefore) runsBefore = skippedBefore = 0;   // (as in PublishLive)
        const uint64_t newRuns = runs - runsBefore, newSkipped = skipped - skippedBefore;
        runsBefore = runs; skippedBefore = skipped;
        d.model = true; d.modelMs = static_cast<float>(g_avgModelMs); d.startMs = static_cast<float>(st->startMs);
        d.keepUpPct = newRuns + newSkipped ? static_cast<float>(100.0 * newRuns / static_cast<double>(newRuns + newSkipped)) : 100.0f;
        d.autoOn = autoOn; d.floor = floor;
        { std::lock_guard<std::mutex> lock(g_autoMutex); d.scale = g_auto.Scale(); d.runEvery = autoOn ? g_auto.RunEvery() : 1; }
    }
    if (!st) { d.upscaler = true; d.upscalerMs = upscalerMs; d.motionMs = motionMs; }
    std::vector<nr::diag::Finding> f = nr::diag::Diagnose(d);
    std::lock_guard<std::mutex> lock(g_textMutex);
    g_findings = std::move(f);
}

// Video memory (vram.h): this process's figure on the card, logged before and after an engine starts (the difference is what the engine holds) and with the periodic lines.
uint64_t g_vramBaseMb = 0; LUID g_vramLuid{};
void LogVideoMemory(const char* what) {
    uint64_t budget = 0; const uint64_t now = nr::ProcessVideoMemoryMb(g_vramLuid, &budget);
    if (!now) return;
    Log("video memory: this process holds %llu MB on the card (%s)%s%llu MB, of a budget of %llu MB", static_cast<unsigned long long>(now), what,
        g_vramBaseMb ? ", " : "", static_cast<unsigned long long>(g_vramBaseMb ? now - std::min(now, g_vramBaseMb) : 0), static_cast<unsigned long long>(budget));
}

void LogProgress(const NrStats& st) {
    // counted in the frames the model was given: Lossless Scaling's captures, or the presented frames with frame generation off (where the
    // capture count stays put, and would log every frame)
    const uint64_t taps = g_presentMode ? g_presentIndex : g_tap.Taps();
    if (!taps) return;
    if (taps == 1 || taps == 60 || taps % 300 == 0)
        Log("%s #%llu: model %.1f ms (avg %.1f), run %.1f ms, GPU start +%.1f done +%.1f ms after submit, tap CPU %.2f ms, interval %.1f ms, runs %llu skipped %llu (two at once %llu), fails %llu | presents %llu (%s), composed %llu, compose CPU %.2f ms, last delta frame %llu offset %.2f",
            g_presentMode ? "presented frame" : "tap", (unsigned long long)taps, st.nrMs, g_avgModelMs, st.totalMs, st.startMs, st.doneMs, g_bridge.CpuMs(), g_bridge.IntervalMs(), (unsigned long long)g_bridge.Runs(),
            (unsigned long long)g_bridge.Skipped(), (unsigned long long)g_bridge.Doubled(), (unsigned long long)st.fails, (unsigned long long)g_lsPresents, g_tap.PresentPattern(), (unsigned long long)g_composed,
            g_compose.CpuMs(), (unsigned long long)g_lastDelta, g_lastOffset);
    if (!g_presentMode && (taps == 60 || taps % 300 == 0))
        Log("motion vectors so far: this frame's flow %llu, the previous frame's %llu, frames dropped waiting for a flow pass %llu",
            (unsigned long long)g_tap.FreshRuns(), (unsigned long long)g_tap.StaleRuns(), (unsigned long long)g_tap.DroppedWaiting());
    if (taps % 300 == 0) {   // the spread of the game's frame times over the last 300 frames (the line above is smoothed)
        float p50, p95, p99, worst; int n, over20, over33;
        if (g_bridge.TakeFrameTimeWindow(p50, p95, p99, worst, n, over20, over33)) {
            g_host->PublishMetric(kAddonId, "frame_p50_ms", p50, "ms");
            g_host->PublishMetric(kAddonId, "frame_p95_ms", p95, "ms");
            g_host->PublishMetric(kAddonId, "frame_p99_ms", p99, "ms");
            UpdateFindings(n, p50, p95, p99, &st);
            Log("frame time over the last %d frames: p50 %.1f ms, p95 %.1f, p99 %.1f, worst %.1f | %d frames over 20 ms (%.0f%%), %d over 33 ms | model %.1f ms, GPU start +%.1f ms",
                n, p50, p95, p99, worst, over20, 100.0 * over20 / n, over33, st.nrMs, st.startMs);
        }
    }
    if (taps == 60 || taps % 1200 == 0) {   // Lossless Scaling's side of the work, on its own queue
        double sub[GpuTimer11::kMarks], com[GpuTimer11::kMarks]; uint64_t subs = 0, coms = 0;
        const bool haveSub = g_bridge.SubmitTimes().Take(sub, subs), haveCom = g_bridge.ComposeTimes().Take(com, coms);
        if (haveSub || haveCom)
            Log("GPU on Lossless Scaling's queue (ms): handing over %.3f (waiting for the run before %.3f, copies %.3f; %llu frames) | compose %.3f "
                "(waiting for its result %.3f, the pass %.3f; %llu presents)", sub[0], sub[1], sub[2], (unsigned long long)subs, com[0], com[1], com[2],
                (unsigned long long)coms);
    }
    if (taps == 60 || taps % 3000 == 0) LogVideoMemory("with the periodic lines: the figure now, then what it added since the engine started");
    if (taps == 60) {
        PresentHook::DumpState([](const char* m) { Log("%s", m); });
        Log("present stages: hook hits %u, body %llu, ready %llu, noted %llu, targeted %llu, with delta %llu", PresentHook::Hits(), (unsigned long long)g_presentStages[0],
            (unsigned long long)g_presentStages[1], (unsigned long long)g_presentStages[2], (unsigned long long)g_presentStages[3], (unsigned long long)g_presentStages[4]);
    }
}

std::atomic<bool> g_lastPassGenerated{ false };   // the last NIS pass showed a generated frame
// The comparison of the upscalers (compare.h): when NIS is the mode shown, this addon (if it is the lowest loaded) labels NIS's picture right after NIS drew it
struct ComparePending { bool valid = false; uint32_t x = 0, y = 0, w = 0, h = 0, enc = 0; float white = 200.0f; } g_comparePending;
std::atomic<IDXGISwapChain*> g_fgChain{ nullptr };   // Lossless Scaling's output swap chain as NIS's back buffer names it (the upscalers' frame generation)
void OnPresent(IDXGISwapChain* sc, UINT sync, UINT flags);
void Compose(IDXGISwapChain* sc);
bool g_composedNow = false;   // the last Compose put the model's result on its frame (the pair's enhanced picture waits for one that did)
void PresentTap(IDXGISwapChain* sc);
void FollowRuntimeChoice();   // further down, with the upscalers
void FollowSharedRecorder();

// A frame for the recorder (under g_frameMutex, on the render thread): its settings follow the panel's, and for the tests it saves by itself
// once recordSaveAfter frames are held.
void Record(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, uint32_t source, uint32_t content = 0, uint32_t tag = 0, bool crop = false) {
    static const bool logSet = (g_recorder.SetLog([](const char* m) { Log("%s", m); }), true);
    (void)logSet;
    bool on; float seconds; int budget, saveAfter;
    { std::lock_guard<std::mutex> lock(g_settingsMutex); on = g_config.recordOn; seconds = g_config.recordSeconds; budget = g_config.recordBudgetMb; saveAfter = g_config.recordSaveAfter; }
    g_recorder.Configure(on, seconds, static_cast<uint32_t>(budget));
    g_recorder.Offer(ctx, frame, source, content, tag, crop ? 1920 : 0, crop ? 1080 : 0);
    static bool savedForTest = false;
    if (saveAfter > 0 && !savedForTest && g_recorder.GetStatus().frames >= static_cast<uint32_t>(saveAfter)) { savedForTest = true; SaveRecording(); }
}

// What frames of this format hold, on the display the chain is on (hdr.h), with the SDR white; logged when it changes.
nr::FrameEncoding FrameEncodingOf(DXGI_FORMAT format, IDXGISwapChain* chain, float* white, ID3D11Device* device = nullptr) {
    int setting;
    { std::lock_guard<std::mutex> lock(g_settingsMutex); setting = g_config.frameEncoding; }
    const nr::DisplayHdr display = nr::QueryDisplayHdr(chain, device ? device : g_tapDevice);
    const nr::FrameEncoding e = nr::EncodingOf(Bridge::ViewFormat(format), setting, display.hdr);
    *white = display.whiteNits;
    static uint64_t said = ~0ull;
    const uint64_t key = (uint64_t)e << 48 | (uint64_t)format << 32 | (uint32_t)display.whiteNits | (display.hdr ? 1ull << 31 : 0);
    if (key != said) {
        said = key;
        Log("frames of format %d are %s (display in %s, SDR white %.0f nits%s)", (int)format, nr::EncodingName(e), display.hdr ? "HDR" : "SDR",
            display.whiteNits, setting ? ", set by hand" : "");
        char text[96];
        if (e == nr::FrameEncoding::Sdr) snprintf(text, sizeof text, "SDR");
        else snprintf(text, sizeof text, "%s, SDR white %.0f nits", nr::EncodingName(e), display.whiteNits);
        std::lock_guard<std::mutex> lock(g_textMutex); g_encodingText = text;
    }
    return e;
}
void FollowFrameGeneration(bool allowed);
void ScalerPresentGuarded(IDXGISwapChain* sc);
void ScalerMarkerGuarded(IDXGISwapChain* sc);

// The engine runs on the card Lossless Scaling's frames come from. Lossless Scaling makes devices on every card and may run a pass on more than
// one for a moment, so it moves only after frames have kept coming from another card for a while.
bool EngineOnFrameCard(const LUID& card) {
    g_frameCard = card; g_frameCardKnown = true;
    if (g_engine.IsReady() && g_engineCardKnown && SameCard(g_engineCard, card)) { g_candidateTaps = 0; return true; }
    if (g_engineStarting) return false;
    if (g_engine.IsFailed() && g_failedCardKnown && SameCard(g_failedCard, card)) return false;   // it cannot run there: wait for Lossless Scaling to move
    if (g_engineCardKnown) {
        if (!SameCard(g_candidateCard, card)) { g_candidateCard = card; g_candidateTaps = 0; }
        if (++g_candidateTaps < kFollowAfterTaps) return false;
    }
    g_candidateTaps = 0;
    Log("engine follows the LSFG device: starting on LUID %08x:%08x", card.HighPart, card.LowPart);
    StartEngine(card);
    return false;   // the table keeps filling while the model loads
}

// One tapped pass (under g_frameMutex). Never skips Lossless Scaling's own dispatch.
void AfterHandOver(bool started, float ceiling, const AutoQuality::Settings& autoSettings, float watchdogMs);
void Tap(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z) {
    TapDecision d;
    if (!g_tap.Observe(ctx, x, y, z, d) || !g_tapDevice) { ReleaseDecision(d); return; }
    if (d.isTap) nr::trace::Add(nr::trace::kTap, 0, 0, static_cast<int32_t>(g_tap.Taps()));
    if (g_presentMode) {
        g_presentMode = false; g_bridge.Shutdown(); g_tapsSeen = g_tap.Taps(); g_presentsWithoutTap = 0;
        Log("frame generation is on: the model takes Lossless Scaling's captured frames again (it took %llu presented frames; %llu composes had their "
            "own frame's result, %llu the frame before's)", (unsigned long long)g_presentIndex, (unsigned long long)g_presentOwn, (unsigned long long)g_presentEarlier);
    }
    LUID card{};
    for (const SeenDevice& s : g_seen) if (s.dev == g_tapDevice) card = s.card;
    if (!EngineOnFrameCard(card)) { ReleaseDecision(d); return; }

    auto log = [](const char* m) { Log("%s", m); };
    if (!g_bridge.IsReady() && !g_bridge.Init(g_tapDevice, ctx, &g_engine, log)) { SwitchOff("bridge init failed"); ReleaseDecision(d); return; }
    if (!g_compose.IsReady() && !g_compose.Init(g_tapDevice, log)) { SwitchOff("compose init failed"); ReleaseDecision(d); return; }
    if (!PresentHook::Installed() && !PresentHook::Install(g_tapDevice, OnPresent, log)) { SwitchOff("could not hook dxgi Present"); ReleaseDecision(d); return; }

    D3D11_TEXTURE2D_DESC frame; d.frame->GetDesc(&frame);
    if (frame.Width < 64 || frame.Height < 64) {   // a minimised window or one in the middle of a resize: nothing to do, and no reason to rebuild the model
        static uint64_t loggedAt = 0;
        if (g_tap.Taps() - loggedAt > 600) { loggedAt = g_tap.Taps(); Log("skipping a %ux%u capture (too small)", frame.Width, frame.Height); }
        ReleaseDecision(d); return;
    }
    { char text[96]; snprintf(text, sizeof text, "%ux%u %s slot %d", frame.Width, frame.Height, FormatName(frame.Format), d.frameSlot);
      std::lock_guard<std::mutex> lock(g_textMutex); g_frameText = text; }
    Record(ctx, d.frame, lsrec::kCaptured);   // as it came, before the model sees it
    if (!g_bridge.Ensure(frame.Width, frame.Height, frame.Format)) {
        SetStatus(Bridge::FormatSupported(frame.Format) ? std::string("the frame copy for the model could not be set up (the Logs tab says why)")
                                                        : std::string("unsupported frame format: ") + FormatName(frame.Format));
        ReleaseDecision(d); return;
    }
    { float white; const nr::FrameEncoding e = FrameEncodingOf(frame.Format, g_lsChain, &white); g_engine.SetFrameEncoding(static_cast<uint32_t>(e), white); }

    NrParams p; float watchdogMs; bool lsFirst; AutoQuality::Settings autoSettings; float autoSeed;
    { std::lock_guard<std::mutex> lock(g_settingsMutex); p = g_config.p; watchdogMs = g_config.watchdogMs; lsFirst = g_config.lsFirst; autoSeed = g_config.autoScaleLast;
      autoSettings = { g_config.autoQuality && g_config.model == 0, g_config.autoBudgetMs, g_config.autoFloor, g_config.gpuLimit ? g_config.gpuLimitPercent : 0.0f }; }   // DLAA works on the whole frame
    const float ceiling = p.workingScale;
    if (autoSettings.on) { std::lock_guard<std::mutex> lock(g_autoMutex); g_auto.Seed(autoSeed); if (g_auto.Scale() > 0) p.workingScale = std::min(ceiling, g_auto.Scale()); }   // (from the scale it settled at last time, not a ramp down from the person's)
    g_bridge.SetLsGpuPriority(lsFirst ? 7 : 0);
    g_bridge.ShareMotion(false);   // the generated frames are moved with LSFG's flow
    // Lossless Scaling's pass may have the frame bound as an input: the copy needs it unbound, and it is put back after
    ID3D11ShaderResourceView* bound[8] = {}; ctx->CSGetShaderResources(0, 8, bound);
    ID3D11ShaderResourceView* none[8] = {}; ctx->CSSetShaderResources(0, 8, none);
    const bool reset = g_resetRequested.exchange(false);
    const bool started = g_bridge.Submit(d.frame, d.flow, d.flowW, d.flowH, p, reset, g_tap.Taps());
    if (reset && !started) g_resetRequested = true;   // a frame left out (the model busy, or its turn off under auto quality) did not carry the reset: it goes with the next
    ctx->CSSetShaderResources(0, 8, bound);
    for (ID3D11ShaderResourceView* v : bound) if (v) v->Release();
    ReleaseDecision(d);
    AfterHandOver(started, ceiling, autoSettings, watchdogMs);
}

// After a frame was handed to the model (from the capture tap, or at Present with frame generation off): the counts, auto quality, the
// watchdog and the live numbers (under g_frameMutex).
void AfterHandOver(bool started, float ceiling, const AutoQuality::Settings& autoSettings, float watchdogMs) {
    const NrStats& st = g_engine.Stats();
    nr::trace::Add(nr::trace::kModel, started ? 1 : 0, started ? static_cast<int32_t>(st.startMs * 100.0) : 0, started ? static_cast<int32_t>(st.nrMs * 100.0) : 0);
    if (started) {
        ++g_runs; g_lastModelMs = st.nrMs; g_lastRunMs = st.totalMs; g_lastRunAtMs = GetTickCount64();
        g_avgModelMs = g_avgModelMs == 0 ? st.nrMs : g_avgModelMs * 0.95 + st.nrMs * 0.05;
        if (g_runs == 1) SetStatus("running");
        // auto quality: the scale it picks here is used from the next frame (changing it rebuilds the model)
        float stable = 0;   // the scale it has held for 30 s with the model in budget: kept, so the next session starts there
        const uint64_t now = GetTickCount64();
        { std::lock_guard<std::mutex> lock(g_autoMutex);
        static uint64_t gpuAt = 0; static unsigned gpuNow = 0;   // the card's load, read once a second, only while the limit is asked for
        if (autoSettings.gpuLimit > 0 && now - gpuAt >= 1000) {
            gpuAt = now; std::string card; { std::lock_guard<std::mutex> lock(g_textMutex); card = g_cardName; }
            gpuNow = g_gpuLoad.Percent(now, card); g_gpuPercent = gpuNow;
            if (!g_gpuLoad.Available() && !g_gpuLoadLogged) { g_gpuLoadLogged = true; Log("graphics card limit: the load cannot be read (%s); the limit does nothing", g_gpuLoad.Why().c_str()); }
        } else if (autoSettings.gpuLimit <= 0) { gpuNow = 0; g_gpuPercent = 0; }
        if (g_auto.Update(now, st.nrMs, static_cast<float>(g_bridge.LastIntervalMs()), ceiling, autoSettings, gpuNow) && !g_auto.History().empty() && g_auto.History().back().atMs == now) {
            const AutoQuality::Step& s = g_auto.History().back();
            nr::trace::Add(nr::trace::kAuto, static_cast<int32_t>(s.to * 100.0f + 0.5f), g_bridge.RunEvery(), static_cast<int32_t>(s.modelMs * 100.0f));
            Log("auto: model resolution %.2f -> %.2f (model %.1f ms, budget %.1f ms%s)", s.from, s.to, s.modelMs, autoSettings.budgetMs * g_auto.Pressure(), g_auto.Pressure() < 0.99f ? (g_auto.GpuOver() ? ", tightened: the graphics card is over its limit" : ", tightened: the game's frames are slow") : "");
        }
        if (autoSettings.on) stable = g_auto.StableScale(now, 30000);
        int manualEvery; { std::lock_guard<std::mutex> lock(g_settingsMutex); manualEvery = g_config.modelEvery; }   // the setting "Run the model": every Nth real frame; auto quality may ask for more
        const int autoEvery = autoSettings.on ? g_auto.RunEvery() : 1;
        const int every = std::max(autoEvery, manualEvery);
        if (every != g_bridge.RunEvery()) {
            g_bridge.SetRunEvery(every);
            nr::trace::Add(nr::trace::kAuto, 0, every, 0);
            const char* what = every == 1 ? "every frame" : every == 2 ? "every 2nd frame" : every == 3 ? "every 3rd frame" : "every 4th frame";
            if (autoEvery >= manualEvery) Log("auto: the model now runs on %s (the game's frames are %s%s)", what, every == 1 ? "steady again" : "still slow at the lowest model resolution", "");
            else Log("the model now runs on %s (the setting Run the model)", what);
        }
        }
        static uint64_t lastSavedAt = 0;
        if (stable > 0 && now - lastSavedAt > 60000) {
            Config c; std::vector<Look> looks; bool save = false;
            { std::lock_guard<std::mutex> settings(g_settingsMutex);
              if (std::fabs(stable - g_config.autoScaleLast) >= 0.02f) { g_config.autoScaleLast = stable; c = g_config; looks = g_looks; save = true; } }
            if (save) { lastSavedAt = now; SaveSettings(g_host, kAddonId, c, looks); Log("auto: settled at model resolution %.2f, kept for the next start", stable); }
        }
    }
    if (g_engine.IsFailed()) SwitchOff(st.lastError);
    if (g_host->GetHostVersion() >= 0x010000) PublishLive(st);
    // the watchdog: a model that stays slower than the limit for 30 frames is switched off, and back on 10 s later (a loading screen or a
    // change of focus is no reason to stay off)
    if (st.nrMs > watchdogMs) {
        if (++g_watchdogHits >= 30) { SwitchOff("NR slower than watchdog threshold for 30 frames"); g_offByWatchdog = true; g_backOnAtMs = GetTickCount64() + 10000; }
    } else g_watchdogHits = 0;
    LogProgress(st);
}

int OnFault(unsigned code, const char* where) {
    char text[64]; snprintf(text, sizeof text, "exception 0x%08x in %s", code, where);
    SwitchOff(text);
    return EXCEPTION_EXECUTE_HANDLER;
}
void TapGuarded(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z) {   // no objects here: __try cannot unwind them
    __try { Tap(ctx, x, y, z); } __except (OnFault(GetExceptionCode(), "tap")) {}
}

// Each device that dispatches is judged once: only one on an NVIDIA card is tapped (under g_frameMutex).
bool Tappable(ID3D11DeviceContext* ctx) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    for (const SeenDevice& s : g_seen) if (s.dev == dev) { dev->Release(); return s.nvidia; }
    const Card card = CardOf(dev);
    if (g_seen.size() < 32) g_seen.push_back({ dev, card.nvidia, card.luid });
    if (card.nvidia && g_tapDevice != dev) { g_bridge.Shutdown(); g_compose.Shutdown(); g_tap.Reset(); g_tapDevice = dev; g_lsChain = nullptr; g_otherChain = nullptr; }
    char text[192];
    snprintf(text, sizeof text, "%p on %s (LUID %08x) -> %s", static_cast<void*>(dev), card.name.c_str(), static_cast<unsigned>(card.luid.LowPart), card.nvidia ? "TAPPED" : "ignored");
    if (card.nvidia) { std::lock_guard<std::mutex> lock(g_textMutex); g_tappedDeviceText = text; }
    else if (!g_tapDevice)   // frame generation runs on this card and no NVIDIA one has been seen: say so, naming it (not a guess from device events)
        SetStatus("frame generation runs on " + (card.name.empty() ? std::string("a card") : card.name) + ", not an NVIDIA card: DLSS 5 needs an NVIDIA "
                  "RTX card. Set Lossless Scaling's Preferred GPU to your NVIDIA card.");
    Log("dispatching device %s", text);
    dev->Release();
    return card.nvidia;
}

// Every few seconds, when new kinds of pass have appeared, the whole table goes to the log (under g_frameMutex).
void LogPassTable() {
    static uint64_t loggedAt = 0; static size_t loggedRows = 0;
    const uint64_t now = GetTickCount64();
    if (now - loggedAt < 5000) return;
    loggedAt = now;
    std::vector<DispatchEntry> rows = g_tap.Snapshot();
    if (rows.size() == loggedRows) return;
    loggedRows = rows.size();
    std::sort(rows.begin(), rows.end(), [](const DispatchEntry& a, const DispatchEntry& b) { return a.count > b.count; });
    DispatchSig tick, tap; g_tap.GetRoles(tick, tap);
    Log("--- dispatch table: %zu shapes, %llu dispatches, ticks %llu, taps %llu, roles %s/%s ---", rows.size(), (unsigned long long)g_tap.Dispatches(),
        (unsigned long long)g_tap.Ticks(), (unsigned long long)g_tap.Taps(), tick.Empty() ? "no-tick" : "tick", tap.Empty() ? "no-tap" : "tap");
    for (size_t i = 0; i < rows.size() && i < 40; ++i) {
        const DispatchEntry& e = rows[i];
        Log("  %6u x (%u,%u,%u) %s%s", e.count, e.sig.x, e.sig.y, e.sig.z, PassText(e.sig).c_str(), e.roleAuto == 1 ? " [auto TICK]" : e.roleAuto == 2 ? " [auto TAP]" : "");
    }
}

// Ctrl+Shift + an F key, read at every present with GetAsyncKeyState (which works while the game has focus; the modifier pair keeps them
// off the game's own keys). One action per press.
void ApplyLookNow(const std::string& name, const std::string& data, const char* why) {
    bool rebuild;
    { std::lock_guard<std::mutex> lock(g_settingsMutex); const float before = g_config.p.workingScale; ApplyLook(data, g_config.p); rebuild = g_config.p.workingScale != before; }
    if (rebuild) g_resetRequested = true;
    Config c; std::vector<Look> looks;
    { std::lock_guard<std::mutex> lock(g_settingsMutex); c = g_config; looks = g_looks; }
    SaveSettings(g_host, kAddonId, c, looks);
    ShowMarker(5);
    Log("%s: preset '%s'%s", why, name.c_str(), rebuild ? " (working scale changed: the model rebuilds)" : "");
}

// ---- the upscalers' before / after pair (Ctrl+Shift+F4, or the panel's button): the upscaled picture at the next pass that replaces NIS,
// then NIS's own at the pass after (the upscaler steps aside for that one pass, and the post-dispatch callback takes NIS's result), saved
// as "<game>_<date>_DLSS.png" (or _FSR) and "..._NIS.png". Render thread only, except the request.
std::atomic<bool> g_pairRequested{ false };
int g_pairStep = 0;                  // 0 none; 1 waiting to take the upscaled picture; 2 NIS's next; 3 NIS's taken after its dispatch
int g_pairWaited = 0;                // passes waited in step 1 (no upscaled picture comes when the upscaler is not replacing NIS)
std::wstring g_pairBase;
ID3D11Resource* g_pairOut = nullptr; D3D11_BOX g_pairBox{}; DXGI_FORMAT g_pairFormat = DXGI_FORMAT_UNKNOWN; uint32_t g_pairEncoding = 0; float g_pairWhite = 200.0f;

void SafeReleasePair() { if (g_pairOut) { g_pairOut->Release(); g_pairOut = nullptr; } }
int g_nrPairStep = 0;                // Neural Rendering's pair: 0 none; 1 the enhanced picture at this present; 2 the original at the next
int g_nrPairCompareBefore = 0;       // the compare view the person had, put back after
int g_nrPairWaited = 0;              // presents waited for one with the model's result on it
std::wstring g_nrPairBase;
void ForgetPair() {   // the device is going away (under g_frameMutex)
    SafeReleasePair(); g_pairStep = 0;
    if (g_nrPairStep) { g_compare = g_nrPairCompareBefore; g_nrPairStep = 0; }
}

// The presented picture now, for Neural Rendering's pair: Lossless Scaling's back buffer after the compose. HDR in its SDR view.
bool CaptureShown(IDXGISwapChain* sc, const std::wstring& path) {
    ID3D11Texture2D* buffer = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&buffer))) || !buffer) return false;
    D3D11_TEXTURE2D_DESC d; buffer->GetDesc(&d);
    float white = 200.0f;
    const nr::FrameEncoding e = FrameEncodingOf(d.Format, sc, &white);
    D3D11_BOX box{ 0, 0, 0, d.Width, d.Height, 1 };
    const bool ok = screenshot::Capture(g_bridge.Context(), buffer, box, Bridge::ViewFormat(d.Format), path, static_cast<uint32_t>(e), white);
    buffer->Release();
    return ok;
}

D3D11_BOX PairBox(const NisPass& pass) {   // the part of NIS's output the picture is in (the whole of it, or its output viewport)
    D3D11_BOX b{}; b.left = pass.Partial() ? pass.outX : 0; b.top = pass.Partial() ? pass.outY : 0; b.right = b.left + pass.outW; b.bottom = b.top + pass.outH; b.back = 1;
    return b;
}

void ReadHotkeys() {
    bool on; int keys[7];
    { std::lock_guard<std::mutex> lock(g_settingsMutex);   // only what is needed: a copy of the whole Config would allocate at every present
      on = g_config.hotkeys; keys[0] = g_config.keyAB; keys[1] = g_config.keySplit; keys[2] = g_config.keySharpDn; keys[3] = g_config.keySharpUp; keys[4] = g_config.keyPreset;
      keys[5] = g_config.keyShot; keys[6] = g_config.keyRecord; }
    static bool wasDown[7] = {};
    static int lookCursor = -1;
    const bool modifiers = on && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    for (int i = 0; i < 7; ++i) {
        const bool down = modifiers && keys[i] > 0 && (GetAsyncKeyState(keys[i]) & 0x8000);
        // the upscalers have no split view, looks or screenshots: only before / after and the sharpening keys act there (a look could
        // otherwise overwrite the upscaler's sharpening with one saved for Neural Rendering)
        const bool applies = !kScalerAddon || i == 0 || i == 2 || i == 3 || i == 5 || i == 6;   // (5, the screenshot key: the before / after pair there)
        if (down && !wasDown[i] && applies) {
            nr::trace::Add(nr::trace::kHotkey, i);
            if (i == 0) { g_compare = g_compare == 2 ? 0 : 2; ShowMarker(g_compare == 2 ? 2 : 1); Log("hotkey: %s", g_compare == 2 ? "original only" : "enhanced"); }
            else if (i == 1) { g_compare = g_compare == 1 ? 0 : 1; ShowMarker(g_compare == 1 ? 3 : 1); Log("hotkey: %s", g_compare == 1 ? "split view" : "enhanced"); }
            else if (i == 5) { g_pairRequested = true; Log("hotkey: before / after pair"); }   // no corner square: it would be in the picture
            else if (i == 6) { Log("hotkey: save the recording"); SaveRecording(); }   // no corner square either: it would be recorded
            else if (i == 4) {
                Look next;
                { std::lock_guard<std::mutex> lock(g_settingsMutex); if (!g_looks.empty()) { lookCursor = (lookCursor + 1) % static_cast<int>(g_looks.size()); next = g_looks[lookCursor]; } }
                if (next.data.empty()) Log("hotkey: no presets saved"); else ApplyLookNow(next.name, next.data, "hotkey");
            } else {
                float sharpen;
                { std::lock_guard<std::mutex> lock(g_settingsMutex); sharpen = g_config.p.sharpen = std::clamp(g_config.p.sharpen + (i == 3 ? 0.05f : -0.05f), 0.0f, 1.0f);
                  if (kScalerAddon && g_config.scalerPerGame) KeepForGame(g_config, g_scalerGame); }
                Config c; std::vector<Look> looks;
                { std::lock_guard<std::mutex> lock(g_settingsMutex); c = g_config; looks = g_looks; }
                SaveSettings(g_host, kAddonId, c, looks);
                ShowMarker(4);
                Log("hotkey: sharpen %.2f", sharpen);
            }
        }
        wasDown[i] = down;
    }
}

// The program in focus, lower case; empty when it is Lossless Scaling itself (its overlay and the manager are in this process) or unknown.
std::string FocusExe(uint32_t* clientW = nullptr, uint32_t* clientH = nullptr) {
    const HWND window = GetForegroundWindow();
    if (!window) return {};
    if (clientW && clientH) { RECT r{}; GetClientRect(window, &r); *clientW = static_cast<uint32_t>(r.right - r.left); *clientH = static_cast<uint32_t>(r.bottom - r.top); }
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid || pid == GetCurrentProcessId()) return {};
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH]; DWORD size = MAX_PATH; std::string exe;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) { const wchar_t* name = wcsrchr(path, L'\\'); exe = Utf8(name ? name + 1 : path); }
    CloseHandle(process);
    for (char& c : exe) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return exe;
}

// A look per program: when a listed program takes focus, its look is applied once (so the person's own changes after that stay).
void FollowFocus() {
    static std::string actedOn;
    const std::string exe = FocusExe();
    if (exe.empty()) return;
    { std::lock_guard<std::mutex> lock(g_textMutex); g_focusExe = exe; }
    if (exe == actedOn) return;
    actedOn = exe;
    std::string lookName, data; bool follow;
    { std::lock_guard<std::mutex> lock(g_settingsMutex);
      follow = g_config.gameAuto;
      for (const auto& [program, look] : g_config.games) if (program == exe) lookName = look;
      for (const Look& l : g_looks) if (!lookName.empty() && l.name == lookName) data = l.data; }
    if (!follow || lookName.empty()) return;
    if (data.empty()) { Log("game %s: its preset '%s' no longer exists", exe.c_str(), lookName.c_str()); return; }
    ApplyLookNow(lookName, data, ("game " + exe + " took focus").c_str());
}

// One present of any swap chain in the process (under g_frameMutex): on Lossless Scaling's, the newest finished delta goes onto the frame.
void Present(IDXGISwapChain* sc) {
    ++g_presents; ++g_presentStages[0];
    if (!g_tapDevice) return;
    ++g_presentStages[1];
    bool ours;   // Lossless Scaling's chain is on the tapped device; two remembered chains, so the manager's window does not make it look again each time
    if (sc == g_lsChain) ours = true;
    else if (sc == g_otherChain) ours = false;
    else {
        ID3D11Device* dev = nullptr;
        sc->GetDevice(IID_PPV_ARGS(&dev));
        ours = dev == g_tapDevice;
        if (dev) dev->Release();
        (ours ? g_lsChain : g_otherChain) = sc;
        Log("present: swap chain %p on %s device", static_cast<void*>(sc), ours ? "the tapped" : "another");
    }
    if (!ours) return;
    ++g_lsPresents;
    ReadHotkeys();
    FollowRuntimeChoice();
    if ((g_lsPresents & 31u) == 0) FollowFocus();
    bool presentMode; { std::lock_guard<std::mutex> lock(g_settingsMutex); presentMode = g_config.presentMode; g_presentWait = g_config.presentWait; }
    FollowFrameGeneration(presentMode);
    if (g_presentMode) PresentTap(sc);
    if (!g_bridge.IsReady() || !g_compose.IsReady()) {
        if (g_nrPairStep) { g_compare = g_nrPairCompareBefore; g_nrPairStep = 0; Log("before / after pair: not taken (nothing is composed now)"); }
        return;
    }
    std::string game;
    { std::lock_guard<std::mutex> lock(g_textMutex); game = g_focusExe; }
    // the before / after pair: this present composed as usual (enhanced), the next one untouched (original), each taken right after it
    if (g_pairRequested.exchange(false) && !g_nrPairStep) {
        g_nrPairBase = screenshot::PairBase(game); g_nrPairCompareBefore = g_compare; g_compare = 0; g_nrPairStep = 1; g_nrPairWaited = 0;
    }
    Compose(sc);
    screenshot::Tick(g_bridge.Context());
    if (g_nrPairStep == 1 && !g_composedNow) {   // no result on this present (none finished yet, or not one of Lossless Scaling's): the next
        if (++g_nrPairWaited > 60) { g_compare = g_nrPairCompareBefore; g_nrPairStep = 0; Log("before / after pair not taken: no present had the model's result"); }
    } else if (g_nrPairStep == 1) {
        if (CaptureShown(sc, g_nrPairBase + L"_NR.png")) { g_compare = 2; g_nrPairStep = 2; }
        else { g_compare = g_nrPairCompareBefore; g_nrPairStep = 0; Log("before / after pair: the picture could not be copied"); }
    } else if (g_nrPairStep == 2) {
        if (!CaptureShown(sc, g_nrPairBase + L"_original.png")) Log("before / after pair: the original could not be copied");
        g_compare = g_nrPairCompareBefore; g_nrPairStep = 0;
    }
    screenshot::OnPresent(g_bridge.Context(), sc, game);   // after the compose: the picture as it is shown
}

// The newest finished delta onto the frame about to be shown (Lossless Scaling's swap chain, under g_frameMutex).
// ---- frame generation off: the frame taken at Present
//
// Without frame generation Lossless Scaling runs no capture pass, so the model would have nothing to run on. Once presents keep coming with
// no capture (kQuietPresents), the presented frame itself is handed to the model, just before the compose. The compose then adds the newest
// result that is ready, usually the frame before's, moved along that frame's motion vectors (the run hands them over with the result) to
// where the picture is now: nothing waits, so the model's time is not added before each frame is shown. With presentWait it adds this same
// frame's result instead, waiting for it on the GPU: exact, but the model's whole run sits in front of every present. The frame is the scaled picture (the screen's size), so the working size is taken as for a frame kPresentWidth wide: the model costs what it
// does for the game's frame. When captures come again, the capture path takes over. Each switch starts the bridge afresh (its frame numbers
// differ: capture counts there, presents here).
constexpr int kQuietPresents = 20;          // presents without a capture, and at least kQuietMs: frame generation can present up to 20
constexpr uint64_t kQuietMs = 500;           // frames per real one, so a count alone could mistake a high multiplier for frame generation off
constexpr float kPresentWidth = 1920.0f;
void PresentTap(IDXGISwapChain* sc) {   // under g_frameMutex, on the presenting thread
    LUID card{};
    for (const SeenDevice& s : g_seen) if (s.dev == g_tapDevice) card = s.card;
    if (!EngineOnFrameCard(card)) return;
    ID3D11DeviceContext* ctx = nullptr; g_tapDevice->GetImmediateContext(&ctx);
    auto log = [](const char* m) { Log("%s", m); };
    if (!g_bridge.IsReady() && !g_bridge.Init(g_tapDevice, ctx, &g_engine, log)) { ctx->Release(); SwitchOff("bridge init failed"); return; }
    if (!g_compose.IsReady() && !g_compose.Init(g_tapDevice, log)) { ctx->Release(); SwitchOff("compose init failed"); return; }
    ID3D11Texture2D* buffer = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&buffer)))) { ctx->Release(); return; }
    D3D11_TEXTURE2D_DESC frame; buffer->GetDesc(&frame);
    Record(ctx, buffer, lsrec::kPresented);   // before the compose puts anything on it
    const bool fits = frame.Width >= 64 && frame.Height >= 64 && g_bridge.Ensure(frame.Width, frame.Height, frame.Format);
    if (!fits && frame.Width >= 64 && frame.Height >= 64) SetStatus("frame generation off: the presented frame's format cannot be given to the model");
    if (fits) {
        { float white; const nr::FrameEncoding e = FrameEncodingOf(frame.Format, sc, &white); g_engine.SetFrameEncoding(static_cast<uint32_t>(e), white); }
        { char text[96]; snprintf(text, sizeof text, "%ux%u %s at Present (frame generation off)", frame.Width, frame.Height, FormatName(frame.Format));
          std::lock_guard<std::mutex> lock(g_textMutex); g_frameText = text; }
        NrParams p; float watchdogMs; bool lsFirst; AutoQuality::Settings autoSettings; float autoSeed;
        { std::lock_guard<std::mutex> lock(g_settingsMutex); p = g_config.p; watchdogMs = g_config.watchdogMs; lsFirst = g_config.lsFirst; autoSeed = g_config.autoScaleLast;
          autoSettings = { g_config.autoQuality && g_config.model == 0, g_config.autoBudgetMs, g_config.autoFloor, g_config.gpuLimit ? g_config.gpuLimitPercent : 0.0f }; }
        const float fit = frame.Width > kPresentWidth ? kPresentWidth / static_cast<float>(frame.Width) : 1.0f;
        const float ceiling = p.workingScale * fit;
        p.workingScale = ceiling;
        if (autoSettings.on) { std::lock_guard<std::mutex> lock(g_autoMutex); g_auto.Seed(autoSeed); if (g_auto.Scale() > 0) p.workingScale = std::min(ceiling, g_auto.Scale()); }
        g_bridge.SetLsGpuPriority(lsFirst ? 7 : 0);
        g_bridge.ShareMotion(!g_presentWait);
        // the scaling pass may have left the buffer bound as its output: unbound for the copy, and put back
        ID3D11UnorderedAccessView* uavs[8] = {}; ctx->CSGetUnorderedAccessViews(0, 8, uavs);
        ID3D11UnorderedAccessView* noUavs[8] = {}; ctx->CSSetUnorderedAccessViews(0, 8, noUavs, nullptr);
        // waiting: a frame that comes while the model is still on the one before waits for it on the GPU (each frame gets its own result);
        // not waiting: it is left out, and the one before's result, moved, stands in
        const bool reset = g_resetRequested.exchange(false);
        const bool started = g_bridge.Submit(buffer, nullptr, 0, 0, p, reset, ++g_presentIndex, g_presentWait);
        if (reset && !started) g_resetRequested = true;
        ctx->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
        for (ID3D11UnorderedAccessView* v : uavs) if (v) v->Release();
        AfterHandOver(started, ceiling, autoSettings, watchdogMs);
    }
    buffer->Release(); ctx->Release();
}

// Which path hands frames to the model: the capture pass while it runs, the present when it has stopped (under g_frameMutex).
void FollowFrameGeneration(bool allowed) {
    const uint64_t taps = g_tap.Taps(), now = GetTickCount64();
    if (taps != g_tapsSeen) {
        g_tapsSeen = taps; g_presentsWithoutTap = 0; g_tapSeenAtMs = now;
        if (g_presentMode) {
            g_presentMode = false; g_bridge.Shutdown();
            Log("frame generation is on: the model takes Lossless Scaling's captured frames again (it took %llu presented frames; %llu composes had their "
                "own frame's result, %llu the frame before's)", (unsigned long long)g_presentIndex, (unsigned long long)g_presentOwn, (unsigned long long)g_presentEarlier);
        }
    } else if (!g_presentMode && allowed && ++g_presentsWithoutTap >= kQuietPresents && now - g_tapSeenAtMs >= kQuietMs) {
        g_presentMode = true; g_bridge.Shutdown(); g_presentIndex = g_presentOwn = g_presentEarlier = 0;
        Log("frame generation is off: the model takes the presented frames (%d presents without a capture)", kQuietPresents);
    }
    if (g_presentMode && !allowed) { g_presentMode = false; g_bridge.Shutdown(); Log("frame generation off: taking the presented frames is switched off"); }
}

// Are Lossless Scaling's bars really there? The viewport is worked out from the shapes (a frame fitted into the screen), which is right when Lossless Scaling draws it at its aspect ratio and wrong in its
// stretch mode, where the frame covers the whole screen. A one-pixel strip through the middle of where each bar would be (a copy every 45 presents, read back at the next, never waited for) says: dark all
// along, there is a bar. Until it has said anything the shapes are not trusted (the whole screen, as before 0.9.26).
struct BarProbe {
    ID3D11Texture2D* stage = nullptr; UINT sw = 0, sh = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN; bool pending = false, vertical = false; uint64_t lastAt = 0;
    int bars = -1;   // -1 not known yet, 0 no bars, 1 bars
    ~BarProbe() { if (stage) stage->Release(); }
    static bool Dark(const uint8_t* p, DXGI_FORMAT f) {
        switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM:
            return p[0] < 4 && p[1] < 4 && p[2] < 4;
        case DXGI_FORMAT_R10G10B10A2_UNORM: { uint32_t v; memcpy(&v, p, 4); return (v & 0x3FF) < 16 && ((v >> 10) & 0x3FF) < 16 && ((v >> 20) & 0x3FF) < 16; }
        case DXGI_FORMAT_R16G16B16A16_FLOAT: { uint16_t h[3]; memcpy(h, p, 6); return (h[0] & 0x7FFF) < 0x2400 && (h[1] & 0x7FFF) < 0x2400 && (h[2] & 0x7FFF) < 0x2400; }
        default: return false;
        }
    }
    static UINT BytesPerPixel(DXGI_FORMAT f) { return f == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u; }
    // vw, vh: the viewport's size as shares of the buffer; called at every compose with the buffer and its context
    void Update(ID3D11DeviceContext* ctx, ID3D11Texture2D* buf, const D3D11_TEXTURE2D_DESC& bd, float vw, float vh) {
        const bool vert = vw < 0.985f;   // pillarbox (bars left and right) or letterbox (top and bottom)
        if (pending && stage && sw == (vert ? 2u : bd.Width) && sh == (vert ? bd.Height : 2u) && fmt == bd.Format) {
            D3D11_MAPPED_SUBRESOURCE m{};
            const HRESULT hr = ctx->Map(stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (SUCCEEDED(hr)) {
                const UINT bpp = BytesPerPixel(fmt); uint64_t dark = 0, all = 0;
                for (UINT y = 0; y < sh; ++y) for (UINT x = 0; x < sw; ++x) { ++all; dark += Dark(static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch + static_cast<size_t>(x) * bpp, fmt) ? 1 : 0; }
                ctx->Unmap(stage, 0);
                const int now = all && dark * 100 >= all * 97 ? 1 : 0;
                if (now != bars) Log("compose: the bars %s (the strips through the middle of where they would be are %s)", now ? "are there" : "are not there: Lossless Scaling stretches the picture over the screen", now ? "dark" : "not dark");
                bars = now; pending = false;
            } else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) pending = false;
        }
        const uint64_t now = GetTickCount64();
        if (pending || (bars >= 0 && now - lastAt < 750)) return;   // (45 presents or so)
        const UINT w = bd.Width, h = bd.Height, bpp = BytesPerPixel(bd.Format);
        if (bpp == 0 || !(bd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || bd.Format == DXGI_FORMAT_R10G10B10A2_UNORM || bpp == 4)) return;
        const UINT nw = vert ? 2u : w, nh = vert ? h : 2u;
        if (!stage || sw != nw || sh != nh || fmt != bd.Format) {
            if (stage) { stage->Release(); stage = nullptr; }
            ID3D11Device* dev = nullptr; buf->GetDevice(&dev); if (!dev) return;
            D3D11_TEXTURE2D_DESC sd{}; sd.Width = nw; sd.Height = nh; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = bd.Format; sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            const HRESULT hr = dev->CreateTexture2D(&sd, nullptr, &stage); dev->Release();
            if (FAILED(hr)) { stage = nullptr; return; }
            sw = nw; sh = nh; fmt = bd.Format;
        }
        if (vert) {   // the middle of the left bar, and of the right one
            const UINT xl = static_cast<UINT>(w * (1.0f - vw) * 0.25f), xr = w - 1 - xl;
            D3D11_BOX l{ xl, 0, 0, xl + 1, h, 1 }, r{ xr, 0, 0, xr + 1, h, 1 };
            ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, buf, 0, &l); ctx->CopySubresourceRegion(stage, 0, 1, 0, 0, buf, 0, &r);
        } else {
            const UINT yt = static_cast<UINT>(h * (1.0f - vh) * 0.25f), yb = h - 1 - yt;
            D3D11_BOX t{ 0, yt, 0, w, yt + 1, 1 }, b{ 0, yb, 0, w, yb + 1, 1 };
            ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, buf, 0, &t); ctx->CopySubresourceRegion(stage, 0, 0, 1, 0, buf, 0, &b);
        }
        pending = true; vertical = vert; lastAt = now;
    }
};
BarProbe g_barProbe;

void Compose(IDXGISwapChain* sc) {
    g_composedNow = false;
    const PresentInfo shown = g_tap.NotePresent();
    ++g_presentStages[2];
    if (shown.target < 0 && !g_presentMode) return;
    ++g_presentStages[3];
    ID3D11ShaderResourceView* delta = nullptr; uint32_t dw = 0, dh = 0;
    // frame generation off and waiting: this present's own result (waited for on the GPU); otherwise the newest finished one
    const bool waitForOwn = g_presentMode && g_presentWait;
    const uint64_t deltaFrame = waitForOwn ? g_bridge.QueuedDelta(&delta, &dw, &dh) : g_bridge.NewestDelta(&delta, &dw, &dh);
    if (!deltaFrame) return;
    if (g_presentMode) { if (deltaFrame == g_presentIndex) ++g_presentOwn; else ++g_presentEarlier; }
    // A result the model has not replaced for a while belongs to another picture: frame generation was switched off (Lossless Scaling still
    // presents, but the model has nothing to run on), or the game paused. Added to every frame it would stand still on the screen.
    if (GetTickCount64() - g_lastRunAtMs > kMaxResultAgeMs) return;
    ++g_presentStages[4];
    NrParams p;
    { std::lock_guard<std::mutex> lock(g_settingsMutex); p = g_config.p; }
    const int compare = g_compare;
    const uint32_t marker = GetTickCount64() < g_markerUntil ? g_marker.load() : 0u;
    if (compare == 2 && !marker) return;   // "original only": nothing to add, so no compose pass at all
    ID3D11Texture2D* buffer = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&buffer)))) return;
    uint32_t fw = 0, fh = 0;
    ID3D11Resource* flow = p.useFlow && !g_presentMode ? g_tap.NewestFlow(fw, fh) : nullptr;   // at Present the result is the frame's own: no sliding
    Compose11::Args a;
    a.target = buffer; a.delta = delta; a.flow = flow; a.flowW = fw; a.flowH = fh; a.flowUnit = p.flowUnit;
    // not waiting, the result is usually a frame old: moved along its own frame's motion (a speed that holds for a frame or two)
    a.motion = g_presentMode && !waitForOwn ? g_bridge.MotionOf(deltaFrame) : nullptr;
    a.offset = !g_presentMode ? static_cast<float>(shown.target - static_cast<double>(deltaFrame))
             : waitForOwn ? 0.0f : static_cast<float>(std::min<uint64_t>(g_presentIndex - deltaFrame, 3));
    a.isGen = !g_presentMode && shown.gen;
    {   // the frame is drawn into the buffer by Lossless Scaling at its aspect ratio (a 2560x1440 game on a 5120x1440 screen: black bars each side): the model's change belongs to that area only (issue #13)
        bool fill; { std::lock_guard<std::mutex> lock(g_settingsMutex); fill = g_config.composeFillsScreen; }
        D3D11_TEXTURE2D_DESC bd; buffer->GetDesc(&bd);
        const float fw = static_cast<float>(g_bridge.Width()), fh = static_cast<float>(g_bridge.Height());
        if (!g_presentMode && !fill && fw > 0 && fh > 0 && bd.Width && bd.Height) {
            const float scale = std::min(static_cast<float>(bd.Width) / fw, static_cast<float>(bd.Height) / fh);
            const float vw = std::min(1.0f, fw * scale / bd.Width), vh = std::min(1.0f, fh * scale / bd.Height);
            if (vw < 0.985f || vh < 0.985f) {   // the shapes say bars: the frame fits one way and not the other
              if (ID3D11DeviceContext* bctx = g_bridge.Context()) g_barProbe.Update(bctx, buffer, bd, vw, vh);
              if (g_barProbe.bars == 1) {
                a.viewport[0] = (1.0f - vw) * 0.5f; a.viewport[1] = (1.0f - vh) * 0.5f; a.viewport[2] = vw; a.viewport[3] = vh;
                static float said[2] = {};
                if (said[0] != vw || said[1] != vh) { said[0] = vw; said[1] = vh; Log("compose: the %.0fx%.0f frame is drawn into %ux%u with bars; the model's change is kept to the picture (x %.0f..%.0f)", fw, fh, bd.Width, bd.Height, a.viewport[0] * bd.Width, (a.viewport[0] + vw) * bd.Width); }
              }
            }
        }
    }
    a.intensity = p.composeIntensity; a.maxDelta = p.maxDelta; a.ghostGuard = p.ghostGuard; a.darkGuard = p.darkGuard; a.hiProtect = p.hiProtect; a.debugView = p.debugView;
    a.sharpen = p.sharpen; a.saturation = p.saturation; a.vibrance = p.vibrance; a.brightness = p.brightness; a.contrast = p.contrast; a.gamma = p.gamma;
    a.shadows = p.shadows; a.highlights = p.highlights; a.grain = p.grain; a.grainSize = p.grainSize; a.grainSeed = static_cast<uint32_t>(g_presents);
    a.hudCount = p.hudCount; memcpy(a.hud, p.hud, sizeof a.hud); a.hudFeather = p.hudFeather; a.hudShow = g_showHud;
    a.compare = static_cast<uint32_t>(compare); a.splitPos = g_splitPos; a.marker = marker;
    { D3D11_TEXTURE2D_DESC desc; buffer->GetDesc(&desc); a.encoding = static_cast<uint32_t>(FrameEncodingOf(desc.Format, sc, &a.whiteNits)); }
    // A result that arrives many frames late (the card held up by the game, the model stalled) does not belong to this picture any more: moved that far along the motion it warps it.
    // Every Nth frame's model run (auto quality) leaves results N frames old by design, so the limit grows with N.
    if (!g_presentMode && a.offset > 3.0f + static_cast<float>(g_bridge.RunEvery())) {
        static uint64_t tooOld = 0;
        if (++tooOld == 1 || tooOld % 600 == 0) Log("compose: the model's result is %.1f frames old: left off this picture (%llu times so far); moved that far along the motion it would warp it", a.offset, static_cast<unsigned long long>(tooOld));
        if (flow) flow->Release();
        buffer->Release();
        return;
    }
    g_bridge.BeginDeltaUse(deltaFrame);
    g_composedNow = true;
    t_ownWork = true;
    const bool composed = g_compose.Run(g_bridge.Context(), a);
    t_ownWork = false;
    g_bridge.EndDeltaUse(deltaFrame);
    if (composed) { ++g_composed; g_lastDelta = deltaFrame; g_lastOffset = a.offset; }
    if (flow) flow->Release();
    buffer->Release();
}
void PresentGuarded(IDXGISwapChain* sc) {   // no objects here: __try cannot unwind them
    __try { Present(sc); } __except (OnFault(GetExceptionCode(), "present")) { t_ownWork = false; }
}

// "Record what is shown" for the upscalers: the output swap chain's back buffer as it goes to the screen (after the upscaler, the frames Lossless Scaling made between included),
// instead of the frame going to the upscaler. Not with frame generation of our own, which records its own presents.
std::string g_compareSavePending;   // a mode's recording waiting to be saved (the recorder was still saving the one before): nothing new is recorded meanwhile, so the next mode's frames do not end up in it
void RecordShownFrame(IDXGISwapChain* sc) {
    if (!g_compareSavePending.empty()) return;
    bool on, shown, ownFrameGen; { std::lock_guard<std::mutex> lock(g_settingsMutex); on = g_config.recordOn; shown = g_config.recordShown; ownFrameGen = kFrameGen && g_config.frameGen; }
    if (!on || !shown || ownFrameGen) return;
    if (compare::Active()) { if (!compare::Records(kCompareMode) || !compare::BurstOpen()) return; }   // comparing: one addon records, in bursts, whichever mode is shown (CompareSegments saves each mode's)
    else if (!compare::MyTurn(kCompareMode, kCompareMode)) return;   // (several upscalers loaded: the one that acts records)
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return;
    ID3D11Device* dev = nullptr; back->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr; if (dev) dev->GetImmediateContext(&ctx);
    if (ctx) Record(ctx, back, lsrec::kPresented, 0, g_lastPassGenerated.load() ? lsrec::kMadeBetween : lsrec::kReal, true);   // (the middle 1920x1080: every presented frame in full is more than the card can copy)
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    back->Release();
}

// While comparing, each mode's bursts are saved as a recording of their own when the mode changes (named <game>-<mode>), so that every mode can be looked at by itself.
void CompareSegments() {
    static int recording = -1;
    if (!g_compareSavePending.empty()) {   // the recorder was busy: again, until it takes them
        if (g_recorder.Save(RecordFolder(), g_compareSavePending, true)) { Log("compare: the recording %s is being saved", g_compareSavePending.c_str()); g_compareSavePending.clear(); }
        return;
    }
    const int now = compare::Records(kCompareMode) ? compare::Current() : -1;
    if (now == recording) return;
    const int was = recording; recording = now;
    if (was < 0) return;
    bool on; { std::lock_guard<std::mutex> lock(g_settingsMutex); on = g_config.recordOn; }
    if (!on) return;
    std::string game; { std::lock_guard<std::mutex> lock(g_textMutex); game = g_focusExe; }
    if (game.size() > 4 && game.compare(game.size() - 4, 4, ".exe") == 0) game.resize(game.size() - 4);
    game += std::string("-") + compare::Name(was);
    for (char& c : game) if (c == ' ') c = '_';
    if (g_recorder.Save(RecordFolder(), game, true)) { Log("compare: %s ended; its recording is being saved (%s)", compare::Name(was), game.c_str()); return; }
    if (g_recorder.GetStatus().saving) { g_compareSavePending = game; Log("compare: %s ended; the recorder is still saving the one before, so this recording waits for it", compare::Name(was)); }
    else Log("compare: %s ended; nothing was recorded for it", compare::Name(was));
}

void OnPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (g_off && g_host->GetHostVersion() >= 0x010000) {   // switched off: keep saying so (a status that is not refreshed goes stale)
        static uint64_t saidAt = 0;
        const uint64_t now = GetTickCount64();
        if (now - saidAt >= 1000) {
            saidAt = now;
            std::string why; { std::lock_guard<std::mutex> lock(g_textMutex); why = g_offReason; }
            g_host->SetStatus(kAddonId, ("Switched off: " + why).c_str(), 3);
        }
    }
    if (g_off || g_engineStarting || !sc) return;
    nr::trace::Add(nr::trace::kPresent, static_cast<int32_t>((reinterpret_cast<uintptr_t>(sc) >> 4) & 0x7fffffff), static_cast<int32_t>(flags), static_cast<int32_t>(sync));
    if (kScalerAddon) {
        ScalerPresentGuarded(sc);
        CompareSegments();
        if (sc == g_fgChain.load(std::memory_order_acquire)) RecordShownFrame(sc);   // (before the corner square)
        ScalerMarkerGuarded(sc);   // the corner square after a hotkey
        bool frameGen; { std::lock_guard<std::mutex> lock(g_settingsMutex); frameGen = g_config.frameGen; }
        if (kFrameGen && frameGen && sc == g_fgChain.load(std::memory_order_acquire)) {   // Lossless Scaling's output swap chain only (not the manager's window)
            static const bool runtimeSet = (nr::framegen::SetRuntime(g_addonDir + L"\\fsr\\amd_fidelityfx_dx12.dll"), true);   // the shipped FSR 3.1 (FSR 4's build has no frame generation)
            (void)runtimeSet;
            float white = 80.0f; uint32_t encoding = 0;
            { ID3D11Texture2D* back = nullptr;   // what its frames hold (scRGB or HDR10 on an HDR display), for measuring their motion in their SDR view
              if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&back))) && back) { D3D11_TEXTURE2D_DESC d; back->GetDesc(&d); back->Release(); encoding = static_cast<uint32_t>(FrameEncodingOf(d.Format, sc, &white)); } }
            bool recordShown, guard; { std::lock_guard<std::mutex> lock(g_settingsMutex); recordShown = g_config.recordShown; guard = g_config.frameGenGuard; }
            nr::framegen::SetGuard(guard);
            nr::framegen::ShownFn shown;   // "record what is shown": every frame presented, the frames between and the real ones, tagged
            if (recordShown) shown = [](ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, bool made) { Record(ctx, frame, lsrec::kPresented, 0, made ? lsrec::kMadeBetween : lsrec::kReal); };
            nr::framegen::BeforeRealPresent(sc, sync, flags, encoding, white, [](const char* m) { Log("%s", m); }, shown);
        }
        return;
    }   // the upscaler: its picture over NIS's (Handoff::AtPresent); no Enable of its own
    { std::lock_guard<std::mutex> lock(g_settingsMutex); if (!g_config.enabled) return; }
    if (!OwnsFrames()) return;
    std::lock_guard<std::mutex> lock(g_frameMutex);
    PresentGuarded(sc);
}

} // namespace

// ---- one addon of the pair at a time
//
// A guard from when DLSS 5 Neural Rendering and DLSS 4 DLAA (then a second Neural-Rendering-style addon) both tapped the same passes of Lossless
// Scaling and would have added their results on top of each other: only one works on the frames, the one named in a variable of the process,
// which the DLLs read. The upscaler addons take the NIS pass instead and stay out of it (ClaimFrames returns at once for them, and their frame
// path never asks OwnsFrames). Turning one on takes the frames over (ClaimFrames); the other notices within a quarter of a second, switches itself off (its Enable box
// clears, and is saved that way) and lets its model go, so it holds no video memory.

namespace {
constexpr const char* kOwnerVariable = "ECHO_ADDON_FRAME_OWNER";
std::atomic<uint64_t> g_ownerCheckedAt{ 0 };
std::atomic<bool> g_ownsFrames{ false };

void LetFramesGo() {   // off the frame path: the model and the bridge go, as when Lossless Scaling drops its device
    std::thread([] {
        std::lock_guard<std::mutex> lock(g_frameMutex);
        ForgetDevice();
        if (g_engine.IsReady() || g_engine.IsFailed()) g_engine.Shutdown();
        g_engineCardKnown = false;
    }).detach();
}
} // namespace

std::string FrameOwner() {
    char name[32] = {};
    const DWORD n = GetEnvironmentVariableA(kOwnerVariable, name, sizeof name);
    return n > 0 && n < sizeof name ? name : "";
}
void ClaimFrames() { if (kScalerAddon) return; SetEnvironmentVariableA(kOwnerVariable, kAddonId); g_ownsFrames = true; g_ownerCheckedAt = GetTickCount64(); }
void ReleaseFrames() { if (FrameOwner() == kAddonId) SetEnvironmentVariableA(kOwnerVariable, nullptr); g_ownsFrames = false; }

bool OwnsFrames() {   // the caller has checked that this addon is on
    const uint64_t now = GetTickCount64();
    if (now - g_ownerCheckedAt.load() < 250) return g_ownsFrames;
    g_ownerCheckedAt = now;
    const std::string owner = FrameOwner();
    if (owner.empty()) { ClaimFrames(); return true; }
    if (owner == kAddonId) { g_ownsFrames = true; return true; }
    // the other addon was turned on: this one steps aside
    { std::lock_guard<std::mutex> lock(g_settingsMutex); g_config.enabled = false; }   // so this runs once
    g_ownsFrames = false;
    Log("switched off: %s is on now (only one of the two works on the frames)", ProductNameOf(owner.c_str()));
    SetStatus(std::string("off: ") + ProductNameOf(owner.c_str()) + " is on");
    LetFramesGo();
    return false;
}

void SettleFramesAtStart() {
    if (kScalerAddon) return;   // the upscaler works beside Neural Rendering
    bool on; { std::lock_guard<std::mutex> lock(g_settingsMutex); on = g_config.enabled; }
    if (!on) return;
    const std::string owner = FrameOwner();
    if (owner.empty() || owner == kAddonId) { ClaimFrames(); return; }
    { std::lock_guard<std::mutex> lock(g_settingsMutex); g_config.enabled = false; }
    Log("switched off at start: %s is on (only one of the two works on the frames)", ProductNameOf(owner.c_str()));
}

void ForgetFramePath() { std::lock_guard<std::mutex> lock(g_frameMutex); ForgetDevice(); }

std::string PassText(const DispatchSig& sig) {
    std::string text;
    char view[64];
    for (int i = 0; i < 8; ++i) if (sig.srv[i].valid) { snprintf(view, sizeof view, "S%d:%ux%u %s ", i, sig.srv[i].w, sig.srv[i].h, FormatName(sig.srv[i].fmt)); text += view; }
    for (int i = 0; i < 4; ++i) if (sig.uav[i].valid) { snprintf(view, sizeof view, "U%d:%ux%u %s ", i, sig.uav[i].w, sig.uav[i].h, FormatName(sig.uav[i].fmt)); text += view; }
    return text;
}

// The model loads on a thread of its own (it takes seconds). Detached and tracked by g_engineStarting, cleared on every way out: a std::thread
// kept in a static would still be joinable if the process ends without AddonShutdown, and destroying it then ends the process.
void StartEngine(LUID card) {
    if (g_engineStarting.exchange(true)) return;
    std::thread([card] {
        auto failed = [card](const char* why) {
            Log("%s", why);
            g_failedCard = card; g_failedCardKnown = true;
            SetStatus("engine failed (exception; see the log)");
            g_engineStarting = false;
        };
        try {
            { std::lock_guard<std::mutex> lock(g_frameMutex); g_bridge.Shutdown(); if (g_engine.IsReady() || g_engine.IsFailed()) g_engine.Shutdown(); }
            SetStatus("engine: loading model...");
            { std::lock_guard<std::mutex> lock(g_settingsMutex);
              g_engine.SetModel(g_config.model == 1 ? NrEngine::Model::Dlaa : NrEngine::Model::NeuralRendering, g_config.dlaaPreset == SrEngine::kPresetAuto ? 5u : g_config.dlaaPreset); }   // (its DLSS runs at 1:1: auto is E)
            g_vramLuid = card; g_vramBaseMb = nr::ProcessVideoMemoryMb(card); if (g_vramBaseMb) Log("video memory: this process holds %llu MB on the card before Neural Rendering starts", static_cast<unsigned long long>(g_vramBaseMb));
            const bool ok = g_engine.Init(card, g_addonDir + L"\\" NR_FORWARDER_FILENAME, ModelPath(), g_addonDir, g_lsDir, [](const char* m) { Log("%s", m); });
            if (ok) LogVideoMemory("Neural Rendering started: the figure after, then what it added");
            g_engineCard = card; g_engineCardKnown = true;
            if (!ok) {
                g_failedCard = card; g_failedCardKnown = true;
                Log("engine failed on LUID %08x:%08x; it will start again when LS runs LSFG on another NVIDIA adapter", card.HighPart, card.LowPart);
            }
            SetStatus(ok ? "engine ready" : g_engine.Stats().lastError);
            g_engineStarting = false;
        } catch (const std::exception& e) {
            failed((std::string("engine start threw: ") + e.what()).c_str());
        } catch (...) {
            failed("engine start threw a non-standard exception");
        }
    }).detach();
}

void RestartEngine() {
    g_engineCardKnown = false;
    if (g_frameCardKnown) StartEngine(g_frameCard);
}

void OnDeviceEvent(uint32_t id, const void*, uint32_t, void*) {
    { std::lock_guard<std::mutex> lock(g_frameMutex); ForgetDevice(); }
    if (id == EAM_EVENT_D3D11_DEVICE_CHANGED) return;
    auto* const dev = static_cast<ID3D11Device*>(g_host->GetD3D11Device());
    { std::lock_guard<std::mutex> lock(g_textMutex); g_cardDrivesDisplay = false; g_cardName = "?"; }
    if (!dev) return;
    const Card card = CardOf(dev);
    { std::lock_guard<std::mutex> lock(g_textMutex); g_cardName = card.name; g_cardDrivesDisplay = card.drivesDisplay; }
    Log("device %p on '%s' LUID %08x:%08x display=%d -> %s", static_cast<void*>(dev), card.name.c_str(), card.luid.HighPart, card.luid.LowPart, card.drivesDisplay ? 1 : 0,
        card.nvidia ? "NVIDIA, ok" : kAnyCardScaler ? "not NVIDIA, ok for this upscaler" : "not NVIDIA, ignored");
    // (the upscalers judge the card on the NIS pass itself, which names the card really in use: Lossless Scaling makes devices on others too)
    // (nor does Neural Rendering: the card frame generation really runs on is named from its passes, in Tappable)
    if (kScalerAddon) SetStatus("waiting for Lossless Scaling's NIS pass (Scaling Type: NIS, the game in a window smaller than the screen)");   // once it runs, the upscaler's own line
    else if (!g_engine.IsReady()) SetStatus("waiting for LSFG dispatches");
    // The engine starts from the tap, on the card whose device actually runs LSFG (one card, a hybrid laptop, or either card of a two-card
    // machine), not from these events, which come for every device Lossless Scaling makes.
}

// ---- DLSS as Lossless Scaling's scaler (the DLSS Upscaler; see scaler11.h and engine/sr_engine.h)
//
// Every pass still goes through the frame tap, which follows frame generation's optical flow and tells real frames apart; the NIS pass is
// replaced when DLSS is ready, and runs as usual until then, when DLSS fails, or while the Before / after hotkey shows the original.
//
// NVIDIA's DLSS code runs only on the engine's own D3D12 device (started on a thread of its own: it touches no other device). Lossless
// Scaling's D3D11 device and context are used only here, on its render thread, and only for copies and one fence signal (by default nothing
// waits: the picture shown is the newest DLSS has finished, the frame before's).
// (DLSS run on Lossless Scaling's D3D11 device itself crashed it three times, 2026-09-24.)

namespace {
SrEngine g_sr;                                   // DLSS on our own device; ready once g_srStarting is false
ScalerLink g_link;                               // Lossless Scaling's side: under g_frameMutex, on the render thread only
std::atomic<bool> g_srStarting{ false };
ID3D11Device* g_linkDevice = nullptr;            // the device the link was made on (the link holds it)
constexpr uint32_t kGenStuckAfter = 16;   // generated pictures in a row (frame generation makes at most 7 between real frames) after which the test is taken as not telling them apart
bool g_genStuckLogged = false;
uint64_t g_lightRuns = 0; uint32_t g_genIndex = 0;   // lighter upscaling (a test): generated frames given the lighter run, and the generated ones since the real frame
uint32_t g_nisSinceTap = 0, g_nisPerFrame = 0;   // NIS passes between two real frames: the presents per real frame
// How far one presented picture is along from the one before, as a share of a real frame's step: the time between presents over the time between real frames
// (smoothed). With a whole number of presents to a real frame it is 1 / that number; with Lossless Scaling's adaptive mode (2.4 presents to a frame on a 120 Hz
// screen at 50 fps, so 2 and 3 by turns) a count would alternate between 1/2 and 1/3, 20 % wrong on every frame, and the motion it scales would jitter.
double g_presentDtMs = 0, g_realDtMs = 0; int64_t g_lastPresentQpc = 0;
void NotePresentTime() {
    LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    if (g_lastPresentQpc) { const double ms = (now.QuadPart - g_lastPresentQpc) * 1000.0 / f.QuadPart; if (ms < 100.0) g_presentDtMs = g_presentDtMs > 0 ? g_presentDtMs * 0.95 + ms * 0.05 : ms; }
    g_lastPresentQpc = now.QuadPart;
}
float PresentStepFraction() {
    if (g_nisPerFrame <= 1) return 1.0f;   // no generated frames: each presented picture is a real one
    if (g_presentDtMs > 0 && g_realDtMs > 0) return std::clamp(static_cast<float>(g_presentDtMs / g_realDtMs), 0.1f, 1.0f);
    return 1.0f / g_nisPerFrame;
}
uint64_t g_nisSeen = 0, g_scalerStatusAt = 0, g_upscaled = 0;
uint64_t g_linkTries = 0;   // passes handed to the upscaler since its link to Lossless Scaling's device was made
ScalerSecond g_scalerSecond;   // under g_textMutex
std::string g_scalerBlocked;   // under g_textMutex: why the upscaler is not replacing NIS although the NIS pass is seen (empty: nothing in the way)

void SetScalerBlocked(const std::string& why) {
    std::lock_guard<std::mutex> lock(g_textMutex);
    // (logged when it changes, and no more than once in 30 s for the same text: with a second kind of pass in the frame the flag flips on and off
    // every frame, and one user's log grew by 6,700 identical lines in eight minutes)
    static std::string loggedText; static ULONGLONG loggedAt = 0;
    if (why != g_scalerBlocked && !why.empty() && (why != loggedText || GetTickCount64() - loggedAt > 30000)) {
        Log("%s upscaler: %s", kUpscalerName, why.c_str());
        loggedText = why; loggedAt = GetTickCount64();
    }
    g_scalerBlocked = why;
}
// the game's frame time, from one real frame to the next (frame generation's capture pass), for the log and the Performance tab
int64_t g_lastTapQpc = 0;
std::vector<float> g_frameTimes;

void NoteRealFrame() {
    LARGE_INTEGER now, f; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&f);
    if (g_lastTapQpc) {
        const float ms = static_cast<float>((now.QuadPart - g_lastTapQpc) * 1000.0 / f.QuadPart);
        if (ms < 500.0f) {   // longer is a pause (loading, alt-tab), not a frame
            g_frameTimes.push_back(ms);
            g_realDtMs = g_realDtMs > 0 ? g_realDtMs * 0.95 + ms * 0.05 : ms;
            if (g_host && g_host->GetHostVersion() >= 0x010000) g_host->PublishMetric(kAddonId, "frame_ms", ms, "ms");
        }
    }
    g_lastTapQpc = now.QuadPart;
    if (g_frameTimes.size() >= 600) {   // every 600 real frames: the spread of the game's frame times, and what DLSS cost meanwhile
        std::vector<float> t = g_frameTimes; std::sort(t.begin(), t.end());
        auto at = [&](double q) { return t[std::min(t.size() - 1, static_cast<size_t>(q * t.size()))]; };
        double sum = 0; for (float v : t) sum += v;
        Log("game frame time over %zu real frames: average %.1f ms (%.0f fps), p50 %.1f, p95 %.1f, p99 %.1f, worst %.1f | DLSS %.2f ms a presented frame (motion %.2f), %u presented per real frame (each %.2f of a real step)",
            t.size(), sum / t.size(), 1000.0 * t.size() / sum, at(0.5), at(0.95), at(0.99), t.back(), g_sr.GpuMs(), g_sr.MotionMs(), g_nisPerFrame, PresentStepFraction());
        UpdateFindings(static_cast<int>(t.size()), at(0.5), at(0.95), at(0.99), nullptr, static_cast<float>(g_sr.GpuMs()), static_cast<float>(g_sr.MotionMs()));
        g_frameTimes.clear();
    }
}
std::atomic<uint32_t> g_scaleInW{ 0 }, g_scaleInH{ 0 }, g_scaleOutW{ 0 }, g_scaleOutH{ 0 };

// The runtime the upscaler runs on: the file chosen in the manager's Runtimes list (its + menu sets "fsrRuntime" / "dlssRuntime" to it), else
// the one shipped in the addon's fsr or dlss folder. The engine is given the file's folder (NGX looks for nvngx_dlss.dll there by name).
std::wstring g_srRuntimeDir;   // the folder the engine was started from (empty: not started)
// A runtime that is not the shipped one (a newer or modified one the person chose in the Runtimes list) runs code of someone else's inside Lossless Scaling's process, and one that crashes takes
// Lossless Scaling down with it, on every start. A marker file says "this one is being tried" from when it starts until it has upscaled a few hundred frames or Lossless Scaling closes normally; found
// at the next start, the runtime took the process down, and the shipped one is used until the person chooses another (issue #11: FSR 4.1.1b INT8 on an RX 6600 XT closed Lossless Scaling).
std::wstring RuntimeTrialMarker() { return g_addonDir + L"\\runtime-trial.txt"; }
std::string ReadRuntimeTrial() {
    std::string text; FILE* f = nullptr;
    if (_wfopen_s(&f, RuntimeTrialMarker().c_str(), L"rb") != 0 || !f) return text;
    char buf[1024]; const size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
    text.assign(buf, n);
    return text;
}
bool g_trialWritten = false;   // this session put the marker there (and clears it when the runtime has done its first frames, or at a normal close)
void WriteRuntimeTrial(const std::string& chosen) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, RuntimeTrialMarker().c_str(), L"wb") != 0 || !f) return;
    fwrite(chosen.data(), 1, chosen.size(), f); fclose(f);
    g_trialWritten = true;
}
void ClearRuntimeTrialImpl() { DeleteFileW(RuntimeTrialMarker().c_str()); }

std::wstring ChosenRuntimeDir() {
    std::string chosen = g_host ? g_host->GetConfig(kAddonId, kRuntimeKey, "") : "";
    if (kScalerAddon) {
        const std::string trial = ReadRuntimeTrial();
        if (!trial.empty()) {
            if (trial != chosen) ClearRuntimeTrialImpl();   // another runtime is chosen now (the shipped one, or a different one): the person has moved on from the one that did not start
            else if (!g_trialWritten) {   // (a marker this session wrote is its own trial, running now: not a crash)
                static std::string said;
                if (said != chosen) { said = chosen; Log("%s upscaler: the runtime %s did not finish its last start (Lossless Scaling closed while it was being tried): the shipped one runs instead. Choose the shipped one in the Runtimes list and then this one again to try it again.", kUpscalerName, chosen.c_str()); }
                return g_addonDir + L"\\" + kRuntimeFolderW;
            }
        }
    }
    if (chosen.empty()) return g_addonDir + L"\\" + kRuntimeFolderW;
    std::wstring w(chosen.size(), L'\0');
    w.resize(std::max(0, MultiByteToWideChar(CP_UTF8, 0, chosen.c_str(), (int)chosen.size(), w.data(), (int)w.size())));
    if (GetFileAttributesW(w.c_str()) == INVALID_FILE_ATTRIBUTES) {   // moved or deleted by hand: the shipped one rather than no upscaler at all
        static std::string said;
        if (said != chosen) { said = chosen; Log("%s upscaler: the chosen runtime %s is not there: the shipped one runs", kUpscalerName, chosen.c_str()); }
        return g_addonDir + L"\\" + kRuntimeFolderW;
    }
    const size_t slash = w.find_last_of(L"\\/");
    return slash == std::wstring::npos ? w : w.substr(0, slash);
}

// The DLSS Upscaler tells the process where its runtime is as soon as it loads, so that Neural Rendering, if it starts NGX first, puts that
// folder on NGX's search list too (NGX keeps the first Init's paths: ngx_paths.h, issue #7).
void PublishRuntimeForOthersImpl() {
    if (kScalerAddon && !kFsrScaler && !kXessScaler) nr::ngxpaths::PublishDlssRuntime(ChosenRuntimeDir());
}

// The recorder's settings are the same in every addon of ours (SaveSettings mirrors them into each addon's config): switching it on in the panel of
// an addon that is not the one working on the frames must count too (the owner had it on in the FSR Upscaler while running the DLSS Upscaler
// and nothing was saved). A change made from another addon's panel arrives in this addon's config; taken over here, twice a second.
void FollowSharedRecorder() {
    if (!g_host) return;
    const bool on = std::string(g_host->GetConfig(kAddonId, "recordOn", "0")) == "1";
    const float seconds = std::clamp(static_cast<float>(atof(g_host->GetConfig(kAddonId, "recordSeconds", "5"))), 1.0f, 60.0f);
    const int budget = std::clamp(atoi(g_host->GetConfig(kAddonId, "recordBudgetMb", "3072")), 256, 65536);
    const std::string folder = g_host->GetConfig(kAddonId, "recordFolder", "");
    // only what the stored value CHANGED to since the last look counts: a change made in this addon's own panel is in g_config a moment before it is
    // saved, and the stored value still being the old one then must not undo it
    static bool seenValid = false, seenOn = false; static float seenSeconds = 0; static int seenBudget = 0; static std::string seenFolder;
    const bool unchanged = seenValid && seenOn == on && std::fabs(seenSeconds - seconds) < 0.01f && seenBudget == budget && seenFolder == folder;
    seenValid = true; seenOn = on; seenSeconds = seconds; seenBudget = budget; seenFolder = folder;
    if (unchanged) return;
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    if (on == g_config.recordOn && std::fabs(seconds - g_config.recordSeconds) < 0.01f && budget == g_config.recordBudgetMb && folder == g_config.recordFolder) return;
    Log("recorder: %s in another addon's panel (%.0f s, %d MB): the same here", on ? "switched on" : "settings changed or switched off", seconds, budget);
    g_config.recordOn = on; g_config.recordSeconds = seconds; g_config.recordBudgetMb = budget; g_config.recordFolder = folder;
}

// A new choice in the Runtimes list, followed while running (checked twice a second, under g_frameMutex on the render thread). The upscalers
// stop their engine, and the next pass starts it on the new file; Neural Rendering takes the model file its setting now names and starts again.
void FollowRuntimeChoice() {
    static ULONGLONG checkedAt = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - checkedAt < 500 || !g_host) return;
    checkedAt = now;
    FollowSharedRecorder();
    if (!kScalerAddon) { FollowModelChoice(); return; }
    if (g_srStarting || g_srRuntimeDir.empty()) return;   // not started: it starts on the chosen file anyway
    const std::wstring dir = ChosenRuntimeDir();
    if (_wcsicmp(dir.c_str(), g_srRuntimeDir.c_str()) == 0) return;
    Log("%s upscaler: the runtime in %ls is chosen: the engine starts again on it", kUpscalerName, dir.c_str());
    g_link.Shutdown(); g_linkDevice = nullptr;
    g_sr.Shutdown(); g_sr.ClearFailure();
    g_srRuntimeDir.clear();
}

// Whether Neural Rendering is on (switched on in the manager, and its own switch on): then its Picture controls act on the shown picture,
// after the upscaler, and the upscalers' brightness, contrast and gamma stand aside so the picture is not changed twice. Read every second.
bool NeuralRenderingOnNow() {   // (the panel's thread and the render thread both ask: the answer is kept in atomics)
    static std::atomic<ULONGLONG> checkedAt{ 0 }; static std::atomic<bool> on{ false };
    const ULONGLONG now = GetTickCount64();
    if (checkedAt && now - checkedAt < 1000) return on;
    checkedAt = now;
    if (!g_host) return on = false;
    const std::string managerSwitch = g_host->GetConfig("DLSS5NR01", "_enabled", "");
    const bool enabled = managerSwitch.empty() ? GetModuleHandleW(L"DLSS5NR01.dll") != nullptr : managerSwitch == "1";
    return on = enabled && std::string(g_host->GetConfig("DLSS5NR01", "enabled", "1")) == "1";
}

void StartEngineFor(const LUID& card) {   // the engine's own device only: safe on a thread of its own
    // Once NVIDIA's or AMD's runtime has run in this process, this DLL stays loaded until Lossless Scaling closes: their code keeps state
    // that points into ours (NGX's library is linked in), and a teardown left for the exit must not find freed code. Switching the addon off
    // still stops everything; a newer build of it then takes effect after Lossless Scaling restarts, as with Neural Rendering's Present hook.
    static bool pinned = false;
    if (!pinned) {
        HMODULE self = nullptr;
        pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&StartEngineFor), &self) != 0;
    }
    g_srStarting = true;
    SetStatus(std::string(kUpscalerName) + ": starting...");
    g_srRuntimeDir = ChosenRuntimeDir();
    if (kScalerAddon && g_host) {   // a runtime that is not the shipped one is on trial until it has upscaled a few hundred frames
        const std::string chosen = g_host->GetConfig(kAddonId, kRuntimeKey, "");
        if (!chosen.empty() && g_srRuntimeDir != g_addonDir + L"\\" + kRuntimeFolderW) WriteRuntimeTrial(chosen); else if (g_trialWritten) { ClearRuntimeTrialImpl(); g_trialWritten = false; }   // (a marker left by a runtime that crashed stays: the shipped one is running because of it)
    }
    std::thread([card, dir = g_srRuntimeDir] {
        LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
        const SrEngine::Backend backend = kXessScaler ? SrEngine::Backend::Xess : kFsrScaler ? SrEngine::Backend::Fsr : SrEngine::Backend::Dlss;
        g_vramLuid = card; g_vramBaseMb = nr::ProcessVideoMemoryMb(card);
        const bool ok = g_sr.Init(card, g_addonDir, dir, [](const char* m) { Log("%s", m); }, backend);
        QueryPerformanceCounter(&b);
        Log("%s upscaler: engine %s in %.0f ms, on a thread of its own", kUpscalerName, ok ? "started" : "failed", (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart);
        if (ok) LogVideoMemory("the upscaler started: the figure after, then what it added (its textures are made at the first frame)");
        SetStatus(ok ? std::string(kUpscalerName) + " ready" : g_sr.LastError());
        g_srStarting = false;
    }).detach();
}

// The upscalers' settings per game: when another game takes focus (checked about once a second), its own settings come back; one seen for
// the first time keeps the settings in use, and whatever is changed while it is the game in play is kept for it (Commit). Only the window
// Lossless Scaling is scaling counts: one whose inside is the frame's size (a chat program or a browser in front is not a game).
void FollowScalerGame(uint32_t frameW, uint32_t frameH) {
    uint32_t w = 0, h = 0;
    const std::string exe = FocusExe(&w, &h);   // empty for Lossless Scaling itself (its panel): the game before stays the one in play
    if (exe.empty()) return;
    auto alike = [](uint32_t a, uint32_t b) { return a + 8 >= b && b + 8 >= a; };
    if (!alike(w, frameW) || !alike(h, frameH)) return;
    { std::lock_guard<std::mutex> lock(g_textMutex); g_focusExe = exe; }
    Config c; std::vector<Look> looks; bool known = false;
    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        if (exe == g_scalerGame) return;
        g_scalerGame = exe;
        if (!g_config.scalerPerGame) return;
        for (const auto& [e, p] : g_config.scalerGames) if (e == exe) { ApplyProfile(g_config, p); known = true; }
        if (!known) KeepForGame(g_config, exe);
        c = g_config; looks = g_looks;
    }
    SaveSettings(g_host, kAddonId, c, looks);
    if (known) Log("game %s took focus: its own settings (sharpening %.2f, stability %.2f, edge smoothing %.2f)", exe.c_str(), c.p.sharpen, c.scalerStability, c.scalerEdges);
    else Log("game %s took focus: new to the upscaler; it keeps the settings in use, and any change made now is kept for it", exe.c_str());
}

// ---- the stall monitor: a thread of its own that says, in the log, where Lossless Scaling's render thread stopped when it stays inside the
// upscaler's pass, or no NIS pass comes for a while after they had been coming (Lossless Scaling's queue held up, for instance by a GPU
// wait). Diagnosis only: it changes nothing.
std::atomic<ULONGLONG> g_passEnteredAt{ 0 }, g_passLeftAt{ 0 };
std::atomic<const char*> g_passStep{ "" };

void StartStallMonitor() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            bool saidInside = false, saidQuiet = false;
            for (;;) {
                Sleep(1000);
                const ULONGLONG now = GetTickCount64(), in = g_passEnteredAt.load(), out = g_passLeftAt.load();
                const bool inside = in && in > out;
                if (inside && now - in > 3000) {
                    if (!saidInside) Log("stall monitor: Lossless Scaling's render thread has been inside the upscaler's pass for %llu ms, at: %s / %s; %s",
                                         (unsigned long long)(now - in), g_passStep.load(), g_link.Step(), g_link.Describe().c_str());
                    saidInside = true;
                } else saidInside = false;
                if (!inside && out && g_upscaled && now - out > 3000) {
                    if (!saidQuiet) Log("stall monitor: no pass from Lossless Scaling for %llu ms (normal when scaling stopped; a stall if the picture froze). The last one left at: %s; %s", (unsigned long long)(now - out),
                                        g_passStep.load(), g_link.Describe().c_str());
                    saidQuiet = true;
                } else if (!inside) saidQuiet = false;
            }
        }).detach();
    });
}

struct PassTimer {
    PassTimer() { g_passEnteredAt = GetTickCount64(); g_passStep = "entered"; }
    ~PassTimer() { g_passLeftAt = GetTickCount64(); }
};

bool ScalerPass(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z) {
    StartStallMonitor();
    const PassTimer passTimer;
    std::lock_guard<std::mutex> lock(g_frameMutex);
    g_passStep = "in the pass";
    TapDecision d;
    g_tap.Observe(ctx, x, y, z, d);   // the flow and the real frames (nothing is handed to a model here)
    if (d.isTap) { if (g_nisSinceTap) g_nisPerFrame = g_nisSinceTap; g_nisSinceTap = 0; NoteRealFrame(); }
    ReleaseDecision(d);
    LogPassTable();

    NisPass pass;
    g_passStep = "looking for the NIS pass";
    if (!FindNisPass(ctx, x, y, z, pass, [](const char* m) { Log("%s", m); })) {
        if (NisLayoutRefused())   // NIS is chosen, but for a window it scales into part of the screen in a way that cannot be followed
            SetScalerBlocked(g_upscaled ? "Lossless Scaling also draws a pass into part of the screen that the upscaler leaves alone (NIS stays for that one). Normal while frames are being upscaled: "
                                          "the log's \"frames upscaled\" lines count them."
                                        : "Lossless Scaling scales this window into part of the screen in a way the upscaler cannot follow yet, so NIS stays. "
                                          "A window of the screen's shape (16:9 on a 16:9 screen) or full screen works. The Logs tab has the details: please report it.");
        return false;
    }
    ++g_nisSeen; ++g_nisSinceTap; NotePresentTime();
    g_tap.SetFrameHint(pass.inW, pass.inH);
    {   // for the log, once: which of the pictures after a real frame's tap is the very frame the tap captured (the real one), and whether any generated-frame pass was seen at all
        static uint64_t sameAt[8], seenAt[8]; static bool said = false;
        const unsigned idx = std::min<unsigned>(g_nisSinceTap - 1, 7);
        ++seenAt[idx]; if (pass.in && pass.in == g_tap.LastTapFrame()) ++sameAt[idx];
        if (!said && g_nisSeen >= 900 && g_nisPerFrame >= 2) {
            said = true;
            char t[200]; int n = 0;
            for (unsigned i = 0; i < 8 && i < static_cast<unsigned>(g_nisPerFrame) + 1; ++i) n += snprintf(t + n, sizeof t - n, "%s%llu of %llu", i ? ", " : "", static_cast<unsigned long long>(sameAt[i]), static_cast<unsigned long long>(seenAt[i]));
            Log("%s upscaler: after a real frame's tap, the picture NIS scales is the captured frame itself at: pass 0 / 1 / ...: %s; generated-frame passes seen: %llu (frame %ux%u)", kUpscalerName, t,
                static_cast<unsigned long long>(g_tap.GeneratedPassesSeen()), pass.inW, pass.inH);
        }
    }
    screenshot::Tick(ctx);
    if (g_pairStep == 3) {   // NIS's half is taken right after its own dispatch: still waiting at the next NIS pass, it never came
        Log("before / after pair: NIS's picture was not taken (its pass did not run); only the upscaled one was saved");
        ForgetPair();
    }
    if (g_pairRequested.exchange(false) && !g_pairStep) {
        std::string game; { std::lock_guard<std::mutex> lock(g_settingsMutex); game = g_scalerGame; }
        g_pairBase = screenshot::PairBase(game); g_pairStep = 1; g_pairWaited = 0;
    }
    if (g_pairStep == 2) {   // NIS's half: this pass runs as NIS, and its result is taken right after its dispatch (OnPostPass)
        SafeReleasePair();
        g_pairOut = pass.out; g_pairOut->AddRef();
        g_pairBox = PairBox(pass); g_pairFormat = Bridge::ViewFormat(pass.outFmt); g_pairEncoding = g_link.Encoding(); g_pairWhite = g_link.White();
        g_pairStep = 3;
        ReleaseNisPass(pass);
        return false;
    }
    FollowRuntimeChoice();
    if ((g_nisSeen & 63u) == 1) FollowScalerGame(pass.inW, pass.inH);
    g_scaleInW = pass.inW; g_scaleInH = pass.inH; g_scaleOutW = pass.outW; g_scaleOutH = pass.outH;
    ReadHotkeys();
    compare::Poll();
    const bool myTurn = compare::MyTurn(kCompareMode, kCompareMode);   // (always, unless several upscalers are loaded together)
    { static bool wasMine = true; if (myTurn && !wasMine) g_resetRequested = true; wasMine = myTurn; }   // (back from another mode: its history is not the picture before)
    bool replaced = false;
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    if (!g_srStarting && dev) {
        if (!g_sr.IsReady() && !g_sr.IsFailed()) {
            const Card card = CardOf(dev);
            if (card.nvidia || kAnyCardScaler) { SetScalerBlocked(""); StartEngineFor(card.luid); }   // FSR and XeSS run on any card
            else {
                if (g_nisSeen == 1) SetStatus("waiting: Lossless Scaling's device is not an NVIDIA card");
                SetScalerBlocked("Lossless Scaling runs on " + (card.name.empty() ? std::string("a card") : card.name) +
                                 ", not an NVIDIA card, and DLSS needs an NVIDIA RTX card. Set Lossless Scaling's Preferred GPU to your NVIDIA card, or use the "
                                 "FSR Upscaler, which runs on any card.");
            }
        } else if (g_sr.IsReady()) {
            if (dev != g_linkDevice) {   // Lossless Scaling's (new) device: the link is made on it, here on its render thread
                g_link.ReportDeviceChange();
                g_link.Shutdown();
                g_linkDevice = g_link.Init(dev, ctx, &g_sr, [](const char* m) { Log("%s", m); }) ? dev : nullptr;
                g_linkTries = 0;
            }
            int handoffMode; { std::lock_guard<std::mutex> settings(g_settingsMutex); handoffMode = g_config.scalerHandoff; }
            bool frameGenOn; { std::lock_guard<std::mutex> settings(g_settingsMutex); frameGenOn = kFrameGen && g_config.frameGen; }
            // Present is hooked in every mode: the hotkeys' corner square is drawn there (and, at Present, the picture goes over NIS's)
            if (!PresentHook::Installed() && !PresentHook::Install(dev, OnPresent, [](const char* m) { Log("%s", m); }))
                Log("%s upscaler: could not hook Present; no corner square after a hotkey%s", kUpscalerName,
                    handoffMode == static_cast<int>(ScalerLink::Handoff::AtPresent) || frameGenOn ? ", and the picture cannot go over NIS's there" : "");
            if (g_linkDevice == dev && g_compare.load() != 2 && myTurn) {   // "original only" lets NIS run, for comparing
                NrParams p; unsigned preset; int handoff, motion; bool gpuWait, steadyFast, shapes, easu, reuseMotion, lightGen; float stability, edges, leanFrom, leanRest, steadySharp, moveCut;
                { std::lock_guard<std::mutex> settings(g_settingsMutex); p = g_config.p; preset = g_config.dlaaPreset; handoff = g_config.scalerHandoff; motion = g_config.motionSource;
                  gpuWait = g_config.scalerGpuWait; stability = g_config.scalerStability; edges = g_config.scalerEdges; steadyFast = g_config.scalerFastMotion; shapes = g_config.motionShapes; leanFrom = g_config.scalerLeanFrom; easu = g_config.leanEasu; leanRest = g_config.scalerLeanRest; steadySharp = g_config.scalerSteadySharp; moveCut = g_config.scalerMoveCut; reuseMotion = g_config.scalerReuseMotion; lightGen = g_config.scalerLightGen; }
                g_sr.SetStability(stability); g_sr.SetEdgeSmoothing(edges);
                g_sr.SetFastMotion(steadyFast ? -1.0f : 0.0f); g_sr.SetFastMotionShare(leanFrom * 0.01f);   // in fast motion lean on the frame (from the chosen share, or its own), or never
                g_sr.SetLeanMode(easu ? 1u : 0u); g_sr.SetLeanRest(leanRest); g_sr.SetSteadySharpen(steadySharp); g_sr.SetMoveCut(moveCut);
                g_sr.SetFlowReuse(reuseMotion && !lightGen && g_nisPerFrame >= 2 ? 2 : 0);   // frame generation: every other presented frame keeps the motion estimate of the one before
                g_sr.SetMeanWeight(shapes ? 0.0f : 1.0f); g_sr.SetGradWeight(shapes ? 0.3f : 0.0f);   // the motion matched by shape and edges (a test), or by brightness
                ScalerLink::Picture picture;   // at the defaults while Neural Rendering is on (its own Picture controls act on the shown picture)
                if (!NeuralRenderingOnNow()) {
                    picture.brightness = p.brightness; picture.contrast = p.contrast; picture.gamma = p.gamma; picture.shadows = p.shadows;
                    picture.highlights = p.highlights; picture.saturation = p.saturation; picture.vibrance = p.vibrance;
                }
                g_link.SetPicture(picture);
                g_passStep = "reading the display's HDR state";
                {   // the display asked is the one Lossless Scaling's window is on: NIS writes its swap chain's back buffer, which names its chain
                    // (asked each time and let go at once: a reference kept could stop Lossless Scaling from replacing its swap chain)
                    IDXGISwapChain* chain = nullptr;
                    IDXGISurface* surface = nullptr;
                    if (pass.out && SUCCEEDED(pass.out->QueryInterface(IID_PPV_ARGS(&surface)))) {
                        if (FAILED(surface->GetParent(IID_PPV_ARGS(&chain)))) chain = nullptr;
                        surface->Release();
                    }
                    float white; const nr::FrameEncoding e = FrameEncodingOf(pass.inFmt, chain, &white, dev);
                    g_fgChain.store(chain, std::memory_order_release);   // (only compared with, never used: frame generation's swap chain)
                    if (chain) chain->Release();
                    g_link.SetEncoding(static_cast<uint32_t>(e), white);
                }
                {   // Technical status's frame line (with the encoding FrameEncodingOf decided)
                    static uint64_t shownKey = 0;
                    const uint64_t key = (uint64_t)pass.inW << 40 | (uint64_t)pass.inH << 16 | (uint64_t)pass.inFmt;
                    if (key != shownKey) {
                        shownKey = key;
                        char text[96]; snprintf(text, sizeof text, "%ux%u %s", pass.inW, pass.inH, FormatName(pass.inFmt));
                        std::lock_guard<std::mutex> lock(g_textMutex); g_frameText = text;
                    }
                }
                uint32_t fw = 0, fh = 0;
                ID3D11Resource* flow = motion == 1 ? g_tap.NewestFlow(fw, fh) : nullptr;
                const float fraction = PresentStepFraction();
                // lighter upscaling of generated frames (a test): a generated-frame pass ran since the last NIS pass, so this picture is a generated one; k counts them since the real frame
                // A generated frame is any picture that is not the real one: the real frame's picture is the frame the tap captured, itself (measured live, x3: pass 2 of 3 after a tap, 297 of 297; Lossless Scaling's generated
                // frames are not compute passes the tap could see: 0 seen), so a picture whose input is another texture is a generated one. (Before the first tap nothing is.)
                bool generated = g_tap.Taps() > 0 && g_tap.LastTapFrame() && pass.in != g_tap.LastTapFrame();
                g_tap.TakeGenerated();   // (the flag is not used here: it is cleared so that it cannot go stale)
                g_genIndex = generated ? g_genIndex + 1 : 0;
                if (generated && g_genIndex > kGenStuckAfter) {   // true for every picture: the test does not tell the real frames from the generated ones in this setup, and the lighter run would go on all of them
                    generated = false;
                    if (!g_genStuckLogged) {
                        g_genStuckLogged = true;
                        DispatchSig sig; uint32_t fw0 = 0, fh0 = 0;
                        const bool known = g_tap.LastGenPass(sig, fw0, fh0);
                        Log("%s upscaler: %u pictures in a row were not the frame the tap captured, so the real frames cannot be told from the generated ones here: no picture gets the lighter run. (Last generated-frame pass seen, frame %ux%u: (%u,%u,%u) %s)", kUpscalerName,
                            g_genIndex, fw0, fh0, sig.x, sig.y, sig.z, known ? PassText(sig).c_str() : "none seen");
                    }
                }
                g_lastPassGenerated = generated;   // (for the recording of what is shown, at the Present that follows)
                const bool cheap = lightGen && g_nisPerFrame >= 2 && generated;
                if (cheap && ++g_lightRuns == 1) Log("%s upscaler: lighter upscaling: the first generated frame was given the lighter run (the generated frame %u since the real one, each frame %.2f of a real step)", kUpscalerName, g_genIndex, fraction);
                t_ownWork = true;
                g_passStep = "Upscale";
                replaced = g_link.Upscale(pass, flow, fw, fh, p.flowUnit, fraction, motion == 0, preset, p.sharpen * kScalerSharpenScale, g_resetRequested.exchange(false),
                                          static_cast<ScalerLink::Handoff>(handoff), gpuWait,
                                          cheap, lightGen ? fraction : 1.0f, cheap ? std::min(1.0f, static_cast<float>(g_genIndex) * fraction) : -1.0f);   // (a reset not handed over is kept by the link: ScalerLink::m_pendingReset)
                t_ownWork = false;
                nr::trace::Add(nr::trace::kUpscale, (replaced ? 1 : 0) | (cheap ? 2 : 0), static_cast<int32_t>(g_sr.GpuMs() * 100.0), static_cast<int32_t>(g_sr.MotionMs() * 100.0));
                g_passStep = "the recorder";
                if (ID3D11Texture2D* grabbed = g_link.TakeGrabbed()) {   // (HDR frames go to the upscaler as light: said so in the file)
                    bool shown; { std::lock_guard<std::mutex> settings(g_settingsMutex); shown = g_config.recordShown; }   // (the presents are recorded: our own frame generation's, or the output chain's at Present, RecordShownFrame)
                    if (!shown) Record(ctx, grabbed, lsrec::kNisInput, g_link.Encoding() ? lsrec::kLight : lsrec::kOwnEncoding);   // (else the presents are recorded)
                }
                g_passStep = "after Upscale";   // the frame as DLSS or FSR got it
                if (flow) flow->Release();
                if (replaced) ++g_upscaled;
                if (g_pairStep == 1) {   // the upscaled half: NIS's output as the upscaler just wrote it
                    if (replaced) {
                        const std::wstring path = g_pairBase + L"_" + std::wstring(kRuntimeListW) + L".png";
                        if (screenshot::Capture(ctx, pass.out, PairBox(pass), Bridge::ViewFormat(pass.outFmt), path, g_link.Encoding(), g_link.White())) g_pairStep = 2;
                        else { g_pairStep = 0; Log("before / after pair: the upscaled picture could not be copied"); }
                    } else if (++g_pairWaited > 240) { g_pairStep = 0; Log("before / after pair not taken: the upscaler is not replacing NIS"); }
                }
                if (!replaced && g_sr.IsFailed()) SetStatus(g_sr.LastError());
                ++g_linkTries;
                if (!replaced && g_link.LastRefusedFormat())
                    SetScalerBlocked("this game's frames are in a format the upscalers cannot take (the Logs tab names it), so NIS stays. Please report it.");
                else if (replaced || g_upscaled) SetScalerBlocked("");
                else if (g_linkTries > 240 && (handoff == static_cast<int>(ScalerLink::Handoff::Late) || handoff == static_cast<int>(ScalerLink::Handoff::Wait)))
                    SetScalerBlocked("it is ready but has not replaced a frame yet. The Logs tab says why.");   // (Observe and AtPresent never replace it)
            } else if (g_linkDevice != dev) {
                SetScalerBlocked("it is ready but could not be connected to Lossless Scaling's device. The Logs tab says why.");
            }
        }
    }
    {   // the comparison of the upscalers (compare.h): the label, and the output chain the recording watches
        const bool label = compare::WantsLabel(kCompareMode, kCompareMode), records = compare::Records(kCompareMode);
        const uint32_t ox = pass.Partial() ? pass.outX : 0, oy = pass.Partial() ? pass.outY : 0;
        if (label && replaced) compare::DrawLabel(ctx, ox, oy, pass.outW, pass.outH, g_link.Encoding(), g_link.White());
        else if (dev && !replaced && (label || records)) {   // another mode's picture, or NIS's own: the label goes on after it (OnPostPass)
            IDXGISwapChain* chain = nullptr; IDXGISurface* surface = nullptr;
            if (pass.out && SUCCEEDED(pass.out->QueryInterface(IID_PPV_ARGS(&surface)))) { if (FAILED(surface->GetParent(IID_PPV_ARGS(&chain)))) chain = nullptr; surface->Release(); }
            float white = 200.0f; const nr::FrameEncoding e = FrameEncodingOf(pass.inFmt, chain, &white, dev);
            g_fgChain.store(chain, std::memory_order_release);   // (only compared with: the output chain the recording of what is shown watches)
            if (chain) chain->Release();
            if (label) g_comparePending = { true, ox, oy, pass.outW, pass.outH, static_cast<uint32_t>(e), white };
        }
    }
    if (dev) dev->Release();
    ReleaseNisPass(pass);
    const uint64_t now = GetTickCount64();
    if (g_host && now - g_scalerStatusAt >= 1000) {
        // the last second's numbers for the panel: pictures a second, and the shares shown twice, not handed over, and waited for
        const ScalerLink::Counters n = g_link.Count();
        static ScalerLink::Counters before; static uint64_t beforeAt = 0;
        if (n.passes < before.passes) before = ScalerLink::Counters();   // a new link starts counting again
        const double seconds = beforeAt ? (now - beforeAt) / 1000.0 : 0.0;
        const uint64_t passes = n.passes - before.passes;
        if (seconds > 0.0) {
            std::lock_guard<std::mutex> lock(g_textMutex);
            g_scalerSecond.fps = passes / seconds;
            g_scalerSecond.repeatPct = passes ? 100.0 * (n.repeats - before.repeats) / passes : 0.0;
            g_scalerSecond.skipPct = passes ? 100.0 * (n.skipped - before.skipped) / passes : 0.0;
            g_scalerSecond.waitPct = passes ? 100.0 * (n.waits - before.waits) / passes : 0.0;
            g_scalerSecond.closePct = passes ? 100.0 * (n.closePasses - before.closePasses) / passes : 0.0;
            g_scalerSecond.valid = true;
        }
        before = n; beforeAt = now;
        g_scalerStatusAt = now;
        char text[160];
        if (replaced) {
            snprintf(text, sizeof text, "%s %ux%u -> %ux%u, %.1f ms", kUpscalerName, g_scaleInW.load(), g_scaleInH.load(), g_scaleOutW.load(), g_scaleOutH.load(), g_sr.GpuMs());
            g_host->SetStatus(kAddonId, text, 1);
            if (g_host->GetHostVersion() >= 0x010000) g_host->PublishMetric(kAddonId, kXessScaler ? "xess_ms" : kFsrScaler ? "fsr_ms" : "dlss_ms", g_sr.GpuMs(), "ms");
            SetStatus(text);
        } else if (g_compare.load() == 2) g_host->SetStatus(kAddonId, "Showing Lossless Scaling's NIS (Before / after)", 0);
    }
    if (replaced && g_upscaled == 600 && g_trialWritten) { ClearRuntimeTrialImpl(); g_trialWritten = false; }   // (it has upscaled ten seconds of frames: it does not take the process down at once)
    if (replaced && (g_upscaled == 120 || g_upscaled % 6000 == 0)) LogVideoMemory("with the periodic lines: the figure now, then what it added since the engine started");
    if (replaced && (g_upscaled == 1 || g_upscaled % 3000 == 0))
        Log("%s scaler: %llu frames upscaled, NIS passes seen %llu, %u per real frame, %s %.2f ms%s", kUpscalerName, (unsigned long long)g_upscaled,
            (unsigned long long)g_nisSeen, g_nisPerFrame, kUpscalerName, g_sr.GpuMs(), g_lightRuns ? (", " + std::to_string(g_lightRuns) + " generated frames given the lighter run").c_str() : "");
    return replaced;   // true: Lossless Scaling's NIS pass is skipped, DLSS's picture is in its output
}

// Where an exception happened: the module and the offset in it (and, for an access violation, what was touched), so a fault in the field can be found in the code. The step the pass was at is the link's.
bool ScalerFault(EXCEPTION_POINTERS* info) {
    const unsigned code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    char where[200] = "";
    if (info && info->ExceptionRecord) {
        const void* at = info->ExceptionRecord->ExceptionAddress; HMODULE mod = nullptr; char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(at), &mod) && mod) {
            GetModuleFileNameA(mod, name, sizeof name); const char* slash = strrchr(name, '\\'); memmove(name, slash ? slash + 1 : name, strlen(slash ? slash + 1 : name) + 1);
        }
        const unsigned long long off = reinterpret_cast<unsigned long long>(at) - reinterpret_cast<unsigned long long>(mod);
        if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
            snprintf(where, sizeof where, " at %s+0x%llx (%s address 0x%llx)", name, off, info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]));
        else snprintf(where, sizeof where, " at %s+0x%llx", name, off);
    }
    Log("%s upscaler: exception 0x%08x%s, in the step \"%s\"", kUpscalerName, code, where, g_link.Step());
    char text[64]; snprintf(text, sizeof text, "exception 0x%08x in the DLSS scaler", code);
    SwitchOff(text);
    return true;
}
bool ScalerGuarded(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z) {   // no objects here: __try cannot unwind them
    __try { return ScalerPass(ctx, x, y, z); } __except (ScalerFault(GetExceptionInformation()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { t_ownWork = false; return false; }
}
void ScalerPresent(IDXGISwapChain* sc) {
    std::lock_guard<std::mutex> lock(g_frameMutex);
    if (g_linkDevice) g_link.PresentCopy(sc);
}
void ScalerPresentGuarded(IDXGISwapChain* sc) {   // no objects here: __try cannot unwind them
    __try { ScalerPresent(sc); } __except (ScalerFault(GetExceptionInformation()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {}
}
// The corner square after a hotkey (the same as Neural Rendering's compose pass draws: green the upscaled picture, red the original, amber
// the split), drawn into Lossless Scaling's back buffer at Present, so the Before / after hotkey says which is which in every mode.
void ScalerMarker(IDXGISwapChain* sc) {
    const uint32_t marker = GetTickCount64() < g_markerUntil ? g_marker.load() : 0u;
    if (!marker || sc != g_fgChain.load(std::memory_order_acquire)) return;   // Lossless Scaling's output only (not the manager's window)
    static const float kColours[5][4] = { { 0.10f, 0.85f, 0.20f, 1.0f }, { 0.90f, 0.15f, 0.15f, 1.0f }, { 0.95f, 0.70f, 0.10f, 1.0f },
                                          { 0.15f, 0.45f, 0.95f, 1.0f }, { 0.65f, 0.30f, 0.90f, 1.0f } };
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return;
    ID3D11Device* dev = nullptr; back->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr; if (dev) dev->GetImmediateContext(&ctx);
    ID3D11DeviceContext1* ctx1 = nullptr; if (ctx) ctx->QueryInterface(IID_PPV_ARGS(&ctx1));
    ID3D11RenderTargetView* rtv = nullptr;
    if (ctx1 && SUCCEEDED(dev->CreateRenderTargetView(back, nullptr, &rtv))) {
        D3D11_TEXTURE2D_DESC d{}; back->GetDesc(&d);
        const LONG side = static_cast<LONG>(std::max(12u, d.Width / 150u));
        const D3D11_RECT r{ 0, 0, side, side };
        ctx1->ClearView(rtv, kColours[std::min(marker, 5u) - 1], &r, 1);
        rtv->Release();
    }
    if (ctx1) ctx1->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    back->Release();
}
void ScalerMarkerGuarded(IDXGISwapChain* sc) {   // no objects here: __try cannot unwind them
    __try { ScalerMarker(sc); } __except (ScalerFault(GetExceptionInformation()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {}
}
} // namespace

void PublishRuntimeForOthers() { PublishRuntimeForOthersImpl(); }

ScalerView GetScalerView() {
    ScalerView v;
    v.starting = g_srStarting; v.ready = !v.starting && g_sr.IsReady(); v.failed = !v.starting && g_sr.IsFailed();
    if (v.failed) v.error = g_sr.LastError();
    v.inW = g_scaleInW; v.inH = g_scaleInH; v.outW = g_scaleOutW; v.outH = g_scaleOutH;
    v.gpuMs = v.ready ? g_sr.GpuMs() : 0; v.motionMs = v.ready ? g_sr.MotionMs() : 0; v.runs = g_upscaled; v.nisSeen = g_nisSeen; v.perFrame = g_nisPerFrame;
    { std::lock_guard<std::mutex> lock(g_textMutex); v.second = g_scalerSecond; if (!v.starting && !v.failed) v.blocked = g_scalerBlocked; }
    if (v.ready) v.provider = g_sr.Provider();
    v.preparing = v.ready && g_sr.BusyMs() > 1500;
    return v;
}

void StopScaler() {
    LARGE_INTEGER f, a, b, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    for (int i = 0; i < 500 && g_srStarting; ++i) Sleep(10);
    std::lock_guard<std::mutex> lock(g_frameMutex);   // nothing else takes it now: the callbacks are gone (AddonShutdown)
    const ScalerLink::Counters n = g_link.Count();
    if (n.passes)
        Log("%s upscaler: this link: %llu passes (%.1f%% within %.0f ms of the one before), %llu pictures shown twice (%.1f%%, %llu of them close), %llu frames not "
            "handed over (%.1f%%); waited on the GPU for the next picture %llu times (%.1f%%, %llu close)", kUpscalerName, (unsigned long long)n.passes,
            100.0 * n.closePasses / n.passes, ScalerLink::Counters::kClosePassMs, (unsigned long long)n.repeats, 100.0 * n.repeats / n.passes,
            (unsigned long long)n.closeRepeats, (unsigned long long)n.skipped, 100.0 * n.skipped / n.passes, (unsigned long long)n.waits,
            100.0 * n.waits / n.passes, (unsigned long long)n.closeWaits);
    g_link.Shutdown();   // unblocks the engine's queue (see ScalerLink::Unblock), drains it, releases the shared textures and fences
    g_linkDevice = nullptr;
    QueryPerformanceCounter(&b);
    const bool clean = g_sr.Shutdown();   // our own device (and NVIDIA's or AMD's runtime); left for the process's exit if the GPU is stuck
    QueryPerformanceCounter(&c);
    Log("%s upscaler: %.2f ms a frame on the GPU at the end (motion %.2f ms of it)", kUpscalerName, g_sr.GpuMs(), g_sr.MotionMs());
    Log("%s upscaler stopped: the link in %.0f ms, the engine in %.0f ms%s", kUpscalerName, (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart,
        (c.QuadPart - b.QuadPart) * 1000.0 / f.QuadPart, clean ? "" : " (the GPU had not finished: its teardown is left for Lossless Scaling's exit)");
}

// After each of Lossless Scaling's passes (the upscalers only): NIS's half of a before / after pair, taken right after NIS wrote it.
void OnPostPass(uint32_t, uint32_t, uint32_t, void*) {
    if (kScalerAddon && g_comparePending.valid) {   // NIS's picture, labelled
        const ComparePending p = g_comparePending; g_comparePending.valid = false;
        if (auto* const ctx = static_cast<ID3D11DeviceContext*>(g_host ? g_host->GetDispatchingContext() : nullptr)) compare::DrawLabel(ctx, p.x, p.y, p.w, p.h, p.enc, p.white);
    }
    if (!kScalerAddon || g_pairStep != 3) return;
    auto* const ctx = static_cast<ID3D11DeviceContext*>(g_host ? g_host->GetDispatchingContext() : nullptr);
    std::lock_guard<std::mutex> lock(g_frameMutex);
    if (g_pairStep != 3 || !g_pairOut || !ctx) return;
    if (!screenshot::Capture(ctx, g_pairOut, g_pairBox, g_pairFormat, g_pairBase + L"_NIS.png", g_pairEncoding, g_pairWhite))
        Log("before / after pair: NIS's picture could not be copied");
    SafeReleasePair();
    g_pairStep = 0;
}

bool OnPass(uint32_t x, uint32_t y, uint32_t z, void*) {
    // after a pause, the watchdog switches it back on (three times a session at most); every other reason waits for the person
    if (g_off && g_offByWatchdog && GetTickCount64() >= g_backOnAtMs && g_backOnCount < 3) {
        ++g_backOnCount; g_offByWatchdog = false; g_watchdogHits = 0; g_off = false;
        Log("watchdog: re-armed (%d of 3)", g_backOnCount.load());
    }
    auto* const ctx = static_cast<ID3D11DeviceContext*>(g_host ? g_host->GetDispatchingContext() : nullptr);
    if (t_ownWork || g_off || g_engineStarting || !ctx) return false;
    if (kScalerAddon) return ScalerGuarded(ctx, x, y, z);   // the upscalers: on while the manager has them on (their old Enable setting is ignored)
    { std::lock_guard<std::mutex> lock(g_settingsMutex); if (!g_config.enabled) return false; }
    if (!OwnsFrames()) return false;
    std::lock_guard<std::mutex> lock(g_frameMutex);
    if (!Tappable(ctx)) { ++g_otherPasses; return false; }
    if (!PresentHook::Installed() && g_tapDevice && g_presentHookTriedOn != g_tapDevice) {   // once per device (a failure is not retried every pass)
        g_presentHookTriedOn = g_tapDevice;
        if (!PresentHook::Install(g_tapDevice, OnPresent, [](const char* m) { Log("%s", m); })) Log("could not hook dxgi Present from Lossless Scaling's passes");
    }
    TapGuarded(ctx, x, y, z);
    LogPassTable();
    return false;   // Lossless Scaling's pass always runs
}

void ResetWatchdog() { g_watchdogHits = 0; }

std::wstring RecordFolder() {
    std::string chosen;
    { std::lock_guard<std::mutex> lock(g_settingsMutex); chosen = g_config.recordFolder; }
    if (!chosen.empty()) {
        std::wstring w(chosen.size(), L'\0');
        const int n = MultiByteToWideChar(CP_UTF8, 0, chosen.c_str(), static_cast<int>(chosen.size()), w.data(), static_cast<int>(w.size()));
        w.resize(n > 0 ? n : 0);
        for (wchar_t& ch : w) if (ch == L'/') ch = L'\\';
        return w;
    }
    PWSTR videos = nullptr; std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos)) && videos) folder = videos;
    CoTaskMemFree(videos);
    // Videos moved into OneDrive would upload every recording (gigabytes): the profile's own Videos folder instead
    if (folder.find(L"\\OneDrive") != std::wstring::npos) {
        PWSTR profile = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile)) && profile) folder = std::wstring(profile) + L"\\Videos";
        CoTaskMemFree(profile);
    }
    return (folder.empty() ? g_lsDir : folder) + L"\\Lossless Scaling";
}

// The frame trace as a CSV in the logs folder: frame-trace-<addon>.csv, the latest export (tools/analyze_frame_trace.py reads it).
void ClearRuntimeTrial() { if (g_trialWritten) { ClearRuntimeTrialImpl(); g_trialWritten = false; } }   // (state.h: at a normal close; a marker left by a runtime that crashed stays)

void ExportFrameTrace(const char* why) {
    std::string error;
    const std::wstring path = g_lsDir + L"\\logs\\frame-trace-" + std::wstring(kAddonId, kAddonId + strlen(kAddonId)) + L".csv";
    if (nr::trace::Export(path, &error)) Log("frame trace: written (%s) to %ls", why, path.c_str());
    else Log("frame trace: not written (%s)", error.c_str());
}

void SaveRecording() {
    std::string game;
    { std::lock_guard<std::mutex> lock(g_textMutex); game = g_focusExe; }
    bool on; { std::lock_guard<std::mutex> lock(g_settingsMutex); on = g_config.recordOn; }
    if (!on) {   // the recorder is off (it is one setting for all our addons since 0.9.18): say so, not "nothing recorded"
        Log("recorder: nothing saved: the recorder is off (tick \"Keep the last few seconds ready to save\" in the Recording section of any addon: it applies to all of them)");
        if (g_host) g_host->SetStatus(kAddonId, "Nothing saved: the recorder is off (switch it on under Recording in any addon)", 2);
        return;
    }
    ExportFrameTrace("with a saved recording");
    if (!g_recorder.Save(RecordFolder(), game)) Log("recorder: nothing saved (%s)", g_recorder.GetStatus().saving ? "a save is running" : "nothing recorded yet");
}

bool NeuralRenderingOn() { return NeuralRenderingOnNow(); }

namespace {
std::string Narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::string FileVersion(const std::wstring& path) {
    DWORD handle = 0; const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return {};
    std::vector<unsigned char> data(size); VS_FIXEDFILEINFO* fixed = nullptr; UINT len = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) || !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &len) || !fixed) return {};
    char text[32]; snprintf(text, sizeof text, "%u.%u.%u", HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS), HIWORD(fixed->dwFileVersionLS));
    return text;
}
const wchar_t* RuntimeFileName() { return kRuntimeFileW; }
const char* RuntimeKey() { return kRuntimeKey; }
} // namespace

std::vector<RuntimeChoice> RuntimeChoices() {
    std::vector<RuntimeChoice> list;
    if (!kScalerAddon) return list;
    const std::wstring shipped = g_addonDir + L"\\" + kRuntimeFolderW + L"\\" + RuntimeFileName();
    const std::string shippedVersion = kFsrScaler ? std::string("3.1.4") : FileVersion(shipped);
    list.push_back({ L"", std::string(kUpscalerName) + " " + shippedVersion + " (" + kRuntimeVendor + ", shipped)" });
    WIN32_FIND_DATAW found{};
    const std::wstring folder = g_addonDir + L"\\runtimes\\" + kRuntimeListW;
    const HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (h == INVALID_HANDLE_VALUE) return list;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == L'.') continue;
        const std::wstring dir = folder + L"\\" + found.cFileName, file = dir + L"\\" + RuntimeFileName();
        if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        std::string name;
        if (FILE* about = _wfopen((dir + L"\\ABOUT.txt").c_str(), L"rb")) {   // its own name: the first line
            char line[128] = {};
            if (fgets(line, sizeof line, about)) { name = line; while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) name.pop_back(); }
            fclose(about);
            if (name.size() >= 3 && (unsigned char)name[0] == 0xEF) name.erase(0, 3);
        }
        if (name.empty()) name = std::string(kUpscalerName) + " " + FileVersion(file) + " (" + Narrow(found.cFileName) + ")";
        list.push_back({ file, name });
    } while (FindNextFileW(h, &found));
    FindClose(h);
    return list;
}

std::wstring ChosenRuntimeFile() {
    const std::string chosen = g_host ? g_host->GetConfig(kAddonId, RuntimeKey(), "") : "";
    std::wstring w(chosen.size(), L'\0');
    w.resize(std::max(0, MultiByteToWideChar(CP_UTF8, 0, chosen.c_str(), (int)chosen.size(), w.data(), (int)w.size())));
    for (wchar_t& ch : w) if (ch == L'/') ch = L'\\';
    return w;
}

void ChooseRuntimeFile(const std::wstring& path) {
    if (!g_host) return;
    g_host->SetConfig(kAddonId, RuntimeKey(), Narrow(path).c_str());
    g_host->SaveConfig();
    Log("%s upscaler: runtime chosen in the panel: %s", kUpscalerName, path.empty() ? "the shipped one" : Narrow(path).c_str());
}

// Neural Rendering: the model file the manager's Runtimes list chose ("snippetPath", its + menu), taken into this addon's settings. From the
// render thread and from the panel before it saves anything (else a save of the panel's copy, made in between, would put the old one back);
// whichever sees the change first restarts the engine on it.
void FollowModelChoice() {
    if (kScalerAddon || !g_host) return;
    const std::string chosen = g_host->GetConfig(kAddonId, "snippetPath", "");
    bool changed;
    { std::lock_guard<std::mutex> lock(g_settingsMutex); changed = chosen != g_config.snippetPath; if (changed) g_config.snippetPath = chosen; }
    if (!changed) return;
    Log("the model file is now %s: the engine starts again on it", chosen.empty() ? "the one in Lossless Scaling's folder" : chosen.c_str());
    ScanRequirements();
    RestartEngine();
}

void RequestPair() { g_pairRequested = true; }   // the panel's button (the pair is taken at the next passes)

std::string ScalerEngineText() {
    if (g_srStarting) return "starting";
    bool blocked; { std::lock_guard<std::mutex> lock(g_textMutex); blocked = !g_scalerBlocked.empty(); }
    if (g_sr.IsReady()) return blocked ? "ready, but not replacing NIS (the Upscaling section says why)" : !g_nisSeen ? "ready, waiting for the NIS pass" : "running";
    if (g_sr.IsFailed()) return "failed: " + g_sr.LastError();
    if (blocked) return "not started: something is in the way (the Upscaling section says what)";
    return g_nisSeen ? "not started (the NIS pass is seen)" : "not started: it starts when Lossless Scaling runs its NIS pass";
}

} // namespace nr
