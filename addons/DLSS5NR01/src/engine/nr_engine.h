// NrEngine: runs the model on its own Direct3D 12 device and queue, on the same graphics card as Lossless Scaling's D3D11 device.
//
// Per frame, one command list on one queue: shrink the frame to the working size (the "proxy"), turn LSFG's optical flow into the model's
// motion vectors, run the model (one to four passes), and write the delta (model minus proxy) into the bridge's shared result texture. The
// engine never touches Lossless Scaling's frames: it reads a copy and writes a delta that the D3D11 side applies when a frame is presented.
// Run() makes the queue wait for the fences the bridge names and signals another; the CPU never waits for the GPU, except to reuse a command
// allocator that is still busy, and to let the run in flight finish before a newly made model takes over. The model is made (at the start and
// at every new working size) on a thread and a queue of its own, so its few hundred ms never stall Lossless Scaling's render thread.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>
#include <atomic>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <condition_variable>
#include <deque>
#include <thread>
#include <string>
#include <vector>
#include "forwarder/nr_api.h"
#include "engine/flow_estimator.h"

// The settings. What the model listens to was measured one knob at a time (docs/dlssnr-knobs.md): all of it is read at every run, and only the
// working scale needs the feature made again.
struct NrParams {
    // read by the model
    uint32_t style = 0;           // 0 standard, 1 natural, 2 cinematic (more is taken as 2)
    uint32_t useAutoMask = 1;
    float intensity = 1.0f;       // the model keeps it to 0..1
    float localStructure = 1.0f;  // not limited; negative works, above 10 is garbage
    float localTone = 1.0f;       // not limited
    float skinStructure = -1.0f;  // -1 = the same as local structure (the model's own default)
    bool useFlow = true;          // motion for the model and the compose (off: none, to compare)
    uint32_t modelMotion = 0;     // the model's motion vectors: 0 measured from the frames (FlowEstimator, per pixel), 1 LSFG's optical flow
    float flowUnit = 2.0f;        // one flow unit is 1/flowUnit of a flow-texture pixel (measured 2.0 on Lossless Scaling 3.x)

    // the model's side
    float workingScale = 1.0f;   // the frame is shrunk by this before the model sees it: the one setting that decides the cost (the ceiling for auto quality, on by default since 0.9.28)
    uint32_t passes = 1;          // model runs per frame (1..4), each taking the one before's result as its colour
    float smoothStable = 0.0f;    // where the frame itself is unchanged, the smoothing's history weight goes up to this (at least deltaSmooth; 0: derived from deltaSmooth, 1 - (1 - deltaSmooth) / 4, at most 0.97)
    float deltaSmooth = 0.5f;     // blend of the previous delta (moved along the motion) into the new one, 0 = off, below 1 (on since 0.9.14: the model flickers in motion; 0.4 the owner's pick live, 0.6 too much)

    // the compose, on the D3D11 side when a frame is presented (compose11)
    float composeIntensity = 1.0f;// how much of the delta lands
    float maxDelta = 0.5f;        // limit on |delta|
    float ghostGuard = 0.5f;      // fades the delta where LSFG's two motion fields disagree and as the delta ages; 0 = off
    float darkGuard = 0.04f;      // a pixel darker than this (the picture's own view, 0..1) takes no positive change from the model, fading in up to it; 0 = off (the glow of a bright HUD box on black, the green lean of black)
    float hiProtect = 0.85f;      // fades the delta as the source brightens from here to white (1 = off)
    float sharpen = 0.5f;         // contrast-adaptive sharpening of the presented frame, 0 = off (0.5 by default since 0.9.28)
    float saturation = 1.0f;      // colour intensity, 1 = unchanged, 0 = grey
    float vibrance = 0.0f;        // extra saturation for muted colours only, 0 = off
    float brightness = 0.0f;      // added to every channel (encoded 0..1 values), 0 = unchanged
    float contrast = 1.0f;        // scale around mid-grey, 1 = unchanged
    float gamma = 1.0f;           // mid-tone curve, above 1 brightens mid-tones, 1 = unchanged
    float shadows = 0.0f;         // lifts (+) or deepens (-) the dark tones only, -1..1
    float highlights = 0.0f;      // brightens (+) or recovers (-) the bright tones only, -1..1
    float grain = 0.0f;           // film grain amount, 0 = off
    float grainSize = 1.0f;       // grain cell size in screen pixels (1..4)
    static const int kMaxHud = 6; // rectangles (fractions of the screen) where the picture is left untouched
    uint32_t hudCount = 0;
    float hud[kMaxHud][4] = {};   // left, top, right, bottom, 0..1
    float hudFeather = 0.004f;    // soft edge of those rectangles, as a fraction of the screen
    uint32_t debugView = 0;       // 0 result, 1 original, 2 delta x4, 3 frame role (real/generated), 4 LSFG flow, 5 ghost guard weight

    bool CreateKeysEqual(const NrParams& o) const { return workingScale == o.workingScale; }
    NrTuning Tuning() const { return NrTuning{ style, useAutoMask, 1u, intensity, localStructure, localTone, skinStructure }; }
};

struct NrStats {
    double nrMs = 0, totalMs = 0;   // the last finished run: the model's time, and the whole run's
    double startMs = 0, doneMs = 0; // when that run started and ended on the GPU, from its submission on the CPU (queue wait and contention)
    uint64_t frames = 0, fails = 0; // runs queued, and runs whose model evaluation failed
    uint64_t busySkips = 0;         // frames not run because a new feature was being made (the model cannot do both at once)
    uint32_t builds = 0; double lastBuildMs = 0;   // features made, and how long the last one took (off the frame path)
    int floatSlot = -1;             // the parameter block's float setter slot, as found
    bool hasFlow = false; uint32_t flowW = 0, flowH = 0;   // the LSFG flow now bound
    uint32_t workW = 0, workH = 0;  // the model's input size
    char lastError[256] = {};
};

class NrEngine {
public:
    using LogFn = std::function<void(const char*)>;

    // Which model runs: set before Init (a change needs the engine started again). DLAA needs no model file and always works on the whole frame.
    enum class Model { NeuralRendering = 0, Dlaa = 1 };
    void SetModel(Model model, unsigned dlaaPreset) { m_model = model; m_dlaaPreset = dlaaPreset; }
    Model GetModel() const { return m_model; }

    // The card is the one with this LUID. forwarderPath: nvngx.dll_dlss5nr01.dll; snippetPath: the model file (Neural Rendering only);
    // dataPath: where NGX may write, and where it finds nvngx_dlss.dll for DLAA; lsDir: searched for the model files as well.
    bool Init(const LUID& adapterLuid, const std::wstring& forwarderPath, const std::wstring& snippetPath, const std::wstring& dataPath,
              const std::wstring& lsDir, LogFn log);
    // Stops everything; when the GPU has not finished within the wait, NVIDIA's teardown (which can wait on a stuck queue for ever) is left for
    // the process's exit and the engine stays failed (the addon's DLL is pinned by its Present hook). False then.
    bool Shutdown();
    bool IsReady() const { return m_ready; }
    bool IsFailed() const { return m_failed; }
    const NrStats& Stats() const { return m_stats; }
    ID3D12Device* Device() const { return m_dev; }

    void Drain();   // the engine's thread and queue idle (before shared textures or fences go away); not while that thread is stuck
    ID3D12Resource* OpenSharedTexture(HANDLE h);
    ID3D12Fence* OpenSharedFence(HANDLE h);
    // LSFG's flow for the next runs (borrowed: the bridge drains the engine before it lets go of it); null for none.
    void SetFlowInput(ID3D12Resource* flow, uint32_t w, uint32_t h);
    // What the frames hold (0 SDR, 1 scRGB, 2 HDR10) and the SDR white in nits: the shrink pass gives the model the frame's SDR view.
    void SetFrameEncoding(uint32_t encoding, float whiteNits) { m_encoding = encoding; m_whiteNits = whiteNits > 1.0f ? whiteNits : 200.0f; }

    // Readies the runs for this frame size and format and these settings. A new working size (the frame's size times the working scale)
    // needs the model's feature and the scratch textures made again, which takes a few hundred ms: that happens on a thread of its own, the
    // runs keep the size they have meanwhile, and the new one is taken between two runs. False while there is nothing to run with yet (the
    // first build) or after a failure.
    bool Prepare(uint32_t width, uint32_t height, DXGI_FORMAT frameFormat, const NrParams& params);
    bool Building() const { return m_buildState.load() == kBuilding; }

    // One run from sharedIn (the frame copy) into sharedDelta (both opened from the bridge's handles, both in COMMON). The queue first waits
    // for waitFence >= waitValue (the copy is done) and usedFence >= usedValue (no present still reads sharedDelta), and afterwards signals
    // signalFence = signalValue. True when the model evaluated; Stats().frames counts every run that was queued.
    // sharedMotion (optional, RG16F at the working size): receives the model's motion vectors beside the delta, so a compose can move this
    // result onto a later frame (frame generation off, not waiting for each frame's own result).
    bool Run(ID3D12Resource* sharedIn, ID3D12Resource* sharedDelta, ID3D12Fence* waitFence, uint64_t waitValue, ID3D12Fence* usedFence,
             uint64_t usedValue, ID3D12Fence* signalFence, uint64_t signalValue, bool reset, ID3D12Resource* sharedMotion = nullptr);

    void Log(const char* fmt, ...);   // also used by NGX's log callback

    // ---- the engine's own thread
    // Lossless Scaling's render thread only queues runs (Submit); a thread of the engine's own records them and calls NVIDIA's model (Run),
    // so a runtime that stops there can never hold up Lossless Scaling (as FSR 4.1.1b once froze it from the upscaler's side). The bridge
    // changes the engine's settings (Prepare, SetFlowInput) only while that thread is idle (Busy). A run that could not be queued still
    // signals its "finished" value, so nothing waits for it for ever; RanOk says whether its result may be used.
    struct Job {
        ID3D12Resource* sharedIn; ID3D12Resource* sharedDelta; ID3D12Fence* waitFence; uint64_t waitValue; ID3D12Fence* usedFence; uint64_t usedValue;
        ID3D12Fence* signalFence; uint64_t signalValue; bool reset; ID3D12Resource* sharedMotion;
    };
    void Submit(const Job& job);
    bool Busy();                                       // a run queued or being recorded
    bool WaitNotBusy(DWORD ms) { return WaitWorkerIdle(ms); }   // a short grace for the run before to be recorded
    uint64_t Submitted() const { return m_submitted.load(std::memory_order_acquire); }   // the newest run whose work is on the GPU queue (its signal value)
    bool WaitSubmitted(uint64_t signalValue, DWORD ms);
    bool RanOk(uint64_t signalValue) const { return signalValue && m_okRing[signalValue % kOkRing].load(std::memory_order_acquire) == signalValue; }
    bool CheckStuck();
    // Forgets which runs were submitted and went through, for a bridge or link that starts counting its frames from 1 again (after it was
    // made anew): a record of the earlier numbering must not vouch for a new frame. Only while the engine's thread is idle.
    void ResetTracking() {
        { std::lock_guard<std::mutex> lock(m_jobMutex); if (m_busy || !m_jobs.empty()) return; }
        m_submitted = 0; for (auto& v : m_okRing) v = 0;
    }   // true once a run has been in the model's code for kStuckMs: the engine is then failed ("stopped responding")
    ~NrEngine() { if (m_worker.joinable()) m_worker.detach(); }   // (at the process's exit without a Shutdown: never std::terminate)
private:
    bool m_abandoned = false;   // the engine's thread did not stop (stuck in NVIDIA's code): nothing is torn down or started again until LS restarts
    static constexpr DWORD kStuckMs = 20000;
    static const int kOkRing = 8;
    std::atomic<uint64_t> m_okRing[kOkRing] = {};
    std::atomic<uint64_t> m_submitted{ 0 };
    std::atomic<ULONGLONG> m_busySince{ 0 };
    std::atomic<bool> m_stuck{ false };
    std::thread m_worker; HANDLE m_workerExited = nullptr;
    std::mutex m_jobMutex; std::condition_variable m_jobCv; std::deque<Job> m_jobs; bool m_busy = false, m_stop = false;
    void StartWorker();
    bool StopWorker(DWORD ms);
    bool WaitWorkerIdle(DWORD ms);
    void WorkerLoop();
    FlowEstimator m_estimator;            // the model's motion measured from the proxy (NrParams::modelMotion 0)
    uint32_t m_encoding = 0; float m_whiteNits = 200.0f;
    bool m_estimatedLast = false; uint64_t m_estimates = 0;
    static const int kSlots = 4;          // command allocators in rotation
    static const int kPassDescriptors = 6, kPasses = 3;   // per pass: t0..t3, u0, u1; the passes: shrink, motion, delta
    static const int kDescriptorsPerSlot = kPassDescriptors * kPasses;

    bool CreateQueue(const LUID& luid);
    bool StartNgx();
    bool StartModel();
    bool StartForwarder();
    bool FindFloatSlot();
    bool CreatePipelines();
    void ReleaseScratch();
    void Fail(const char* fmt, ...);
    ID3D12Resource* MakeTexture(uint32_t w, uint32_t h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state);
    void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
    void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
    void Upload(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, uint32_t bytesPerPixel, const void* pixels, std::vector<ID3D12Resource*>& staging);

    // The feature and the scratch textures of one working size.
    struct Scratch {
        uint32_t ww = 0, wh = 0;
        void* feature = nullptr;
        ID3D12Resource* proxy = nullptr; ID3D12Resource* out[2] = {}; ID3D12Resource* history[2] = {}; ID3D12Resource* depth = nullptr; ID3D12Resource* mvec = nullptr;
    };
    enum BuildState { kIdle, kBuilding, kBuilt, kBuildFailed };
    bool StartBuild(uint32_t ww, uint32_t wh, const NrTuning& tuning);
    static DWORD WINAPI BuildThread(void* self);
    void Build();                         // on the build thread
    void EndBuild();                      // waits for the build thread to end (it has, or is about to)
    void Discard(Scratch& s);             // a set the GPU no longer uses
    void TakeBuilt();                     // the built set becomes the one the runs use (on the frame path, between runs)
    bool WaitIdle();                      // false when the GPU did not finish in time
    int TakeSlot();                       // the next allocator, reset for recording; -1 when the GPU still has not finished with it
    void ReadTimes(int slot);
    D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptor(int slot, int pass, int i) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GpuDescriptor(int slot, int pass, int i) const;

    LogFn m_log;
    Model m_model = Model::NeuralRendering; unsigned m_dlaaPreset = 0;
    std::atomic<bool> m_ready{ false }, m_failed{ false };
    NrStats m_stats{};
    std::wstring m_forwarderPath, m_snippetPath, m_dataPath, m_lsDir;

    // Direct3D 12
    ID3D12Device* m_dev = nullptr;
    ID3D12CommandQueue* m_queue = nullptr;
    ID3D12CommandAllocator* m_alloc[kSlots] = {};
    ID3D12GraphicsCommandList* m_list = nullptr;
    ID3D12Fence* m_fence = nullptr; HANDLE m_fenceEvent = nullptr; uint64_t m_fenceValue = 0;
    uint64_t m_slotDone[kSlots] = {};     // the fence value that marks each allocator's last list as finished
    int64_t m_slotSubmitQpc[kSlots] = {};
    int m_nextSlot = 0;
    ID3D12QueryHeap* m_timestamps = nullptr; ID3D12Resource* m_timestampReadback = nullptr; uint64_t m_timestampFreq = 1;

    // the model, through the forwarder
    void* m_caps = nullptr; bool m_ngxJoined = false;   // (ngx_users.h)
    HMODULE m_forwarder = nullptr;
    PFN_nrfwd_probe m_probe = nullptr; PFN_nrfwd_init m_init = nullptr; PFN_nrfwd_set_float_slot m_setFloatSlot = nullptr;
    PFN_nrfwd_probe_float m_probeFloat = nullptr; PFN_nrfwd_get_float m_getFloat = nullptr; PFN_nrfwd_create m_create = nullptr;
    PFN_nrfwd_evaluate m_evaluate = nullptr; PFN_nrfwd_release m_release = nullptr; PFN_nrfwd_last_result m_lastResult = nullptr;
    void* m_feature = nullptr;
    bool m_resetHistory = true;           // the next run tells the model to start its history afresh
    std::mutex m_ngxMutex;                // the model's calls share one parameter block: making a feature and a run never overlap

    // the build thread: its own list and queue, so the feature's GPU setup never sits in front of the runs
    std::atomic<int> m_buildState{ kIdle };
    HANDLE m_buildThread = nullptr;       // a plain thread: one still running at process exit is simply ended, never a std::terminate
    uint32_t m_buildW = 0, m_buildH = 0; NrTuning m_buildTuning{};
    Scratch m_built;                      // written by the build thread, read after m_buildState says kBuilt
    char m_buildError[160] = {};
    ID3D12CommandQueue* m_buildQueue = nullptr;
    ID3D12CommandAllocator* m_buildAlloc = nullptr;
    ID3D12GraphicsCommandList* m_buildList = nullptr;
    ID3D12Fence* m_buildFence = nullptr; HANDLE m_buildEvent = nullptr; uint64_t m_buildFenceValue = 0;

    // the frame and the scratch textures, at the working size
    uint32_t m_w = 0, m_h = 0; DXGI_FORMAT m_fmt = DXGI_FORMAT_UNKNOWN; NrParams m_params{};
    uint32_t m_ww = 0, m_wh = 0;
    ID3D12Resource* m_proxy = nullptr;    // RGBA8: the frame as the model sees it; rests readable
    ID3D12Resource* m_out[2] = {};        // RGBA8: the model's output (more passes go back and forth between the two); rest writable
    ID3D12Resource* m_history[2] = {};    // RGBA16F: the last smoothed delta (smoothing goes back and forth between the two); rest readable
    int m_historyRead = 0; bool m_historyValid = false;
    ID3D12Resource* m_depth = nullptr;    // R32F, flat: the model ignores depth
    ID3D12Resource* m_mvec = nullptr;     // RG16F: zero, or LSFG's flow in working-size pixels; rests readable
    ID3D12Resource* m_flow = nullptr; uint32_t m_flowW = 0, m_flowH = 0;   // LSFG's flow (the bridge's shared copy, borrowed)

    // the three passes of our own
    ID3D12RootSignature* m_rootSig = nullptr;
    ID3D12PipelineState* m_psoShrink = nullptr; ID3D12PipelineState* m_psoMotion = nullptr;
    ID3D12PipelineState* m_psoDelta = nullptr; ID3D12PipelineState* m_psoDeltaSmooth = nullptr;
    ID3D12DescriptorHeap* m_heap = nullptr; uint32_t m_descriptorSize = 0;
};
