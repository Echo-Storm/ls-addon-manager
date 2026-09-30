// SrEngine: an upscaler on a Direct3D 12 device and queue of our own, on the graphics card Lossless Scaling scales on (scaler11.h is the
// Lossless Scaling side): NVIDIA DLSS Super Resolution for the DLSS Upscaler, or AMD FSR 3.1 for the FSR Upscaler (Backend). Both get
// the same inputs: the frame, the motion (measured, or frame generation's), a flat depth and the distrust mask.
//
// NVIDIA's DLSS code never runs on Lossless Scaling's D3D11 device. Run D3D11 DLSS there crashed Lossless Scaling within seconds, three times,
// each in a different place (NVIDIA's driver, NVIDIA's API, Lossless Scaling's own checks); DLSS on a device of our own, as Neural Rendering and
// the DLAA test ran, never did. Lossless Scaling's side only copies textures and signals and waits on fences; everything else happens here.
//
// A run: the queue waits (on the GPU) for "copied", measures the motion from the frames themselves (FlowEstimator) or turns frame
// generation's flow into motion vectors, runs DLSS from the shared input (the frame at the game's size) into the shared output (the picture
// at the screen's size), and signals "done". The CPU never waits for the GPU,
// except to reuse a command allocator that is still busy.
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
#include <string>
#include "engine/flow_estimator.h"

class SrEngine {
public:
    using LogFn = std::function<void(const char*)>;
    enum class Backend { Dlss, Fsr, Xess };
    static constexpr unsigned kPresetAuto = 16;   // the DLSS model chosen by the sizes: E at 1:1 (DLAA), L when upscaling
    ~SrEngine() { if (m_worker.joinable()) m_worker.detach(); }   // (at the process's exit without a Shutdown: never std::terminate)
    // On the card with this LUID; the runtime is looked for in runtimeDir (NVIDIA's nvngx_dlss.dll, or AMD's amd_fidelityfx_dx12.dll).
    // Touches no device but its own: may run on any thread.
    bool Init(const LUID& card, const std::wstring& dataPath, const std::wstring& runtimeDir, LogFn log, Backend backend = Backend::Dlss);
    // False when the GPU had not finished within the wait: then nothing is torn down (NVIDIA's DLSS teardown can wait on a stuck queue for
    // ever); the device and runtime are left for the process's exit, and the engine stays failed (the DLL is pinned, see StartEngineFor).
    bool Shutdown();
    bool IsReady() const { return m_ready; }
    bool IsFailed() const { return m_failed; }
    std::string Provider() const { std::lock_guard<std::mutex> lock(m_providerMutex); return m_provider; }   // FSR: the upscaler the runtime chose ("3.1.4", "4.1.1b"), once running
    void ClearFailure() { if (m_abandoned) return; m_failed = false; std::lock_guard<std::mutex> lock(m_errorMutex); m_error.clear(); }   // after Shutdown: the next Init may try again (another runtime file)
    std::string LastError() const { std::lock_guard<std::mutex> lock(m_errorMutex); return m_error; }

    // ---- the engine's own thread
    // Lossless Scaling's render thread only queues frames (Submit); a thread of the engine's own runs them (Run), so NVIDIA's or AMD's code
    // never runs on Lossless Scaling's thread. A runtime that stopped there froze Lossless Scaling's picture for good (FSR 4.1.1b on the
    // first HDR frame, 2026-09-26); now it only stops the upscaler: NIS goes on, and after kStuckMs the engine is failed and says so.
    struct Job {
        ID3D12Resource* in; uint32_t inW, inH; DXGI_FORMAT inFormat; ID3D12Resource* out; uint32_t outW, outH; DXGI_FORMAT outFormat;
        ID3D12Resource* flow; uint32_t flowW, flowH; float flowUnit, motionFraction; bool estimate; unsigned preset; float sharpen; bool reset;
        bool hdr;   // the frame is an HDR frame as light (1 = the SDR white): the upscaler takes it in its HDR mode and writes light
        ID3D12Fence* copied; uint64_t copiedValue; ID3D12Fence* done; uint64_t doneValue;
    };
    void Submit(const Job& job);   // "done" = job.doneValue is signalled on the engine's queue once it has run (or could not), never before
    // The newest job whose work is on the engine's GPU queue (its done value): only such a frame may be waited for on the GPU.
    uint64_t Submitted() const { return m_submitted.load(std::memory_order_acquire); }
    bool WaitSubmitted(uint64_t doneValue, DWORD ms);   // a short CPU wait for that (the hidden GPU-wait hand-over only)
    bool RanOk(uint64_t doneValue) const { return doneValue && m_okRing[doneValue % kOkRing].load(std::memory_order_acquire) == doneValue; }
    bool CheckStuck();
    // Forgets which runs were submitted and went through, for a bridge or link that starts counting its frames from 1 again (after it was
    // made anew): a record of the earlier numbering must not vouch for a new frame. Only while the engine's thread is idle.
    void ResetTracking() {
        { std::lock_guard<std::mutex> lock(m_jobMutex); if (m_busy || !m_jobs.empty()) return; }
        m_submitted = 0; for (auto& v : m_okRing) v = 0;
    }
    uint64_t BusyMs() const { const ULONGLONG t = m_busySince.load(); return t ? GetTickCount64() - t : 0; }   // how long the job in progress has run (0: none)   // true once a job has been in the runtime's code for kStuckMs: the engine is then failed ("stopped responding")

    ID3D12Resource* OpenSharedTexture(HANDLE h);
    ID3D12Fence* OpenSharedFence(HANDLE h);
    void Drain();   // waits until the queue is idle (before shared textures or fences go away)

    // One upscaled frame. in: the frame (COMMON, the game's size); out: the picture (COMMON, the screen's size, writable); flow: frame
    // generation's flow (COMMON, RGBA16F) or null. The queue waits for copied >= copiedValue first and signals done = doneValue after.
    // motionFraction: the part of a real frame between two presented frames. sharpen: contrast-adaptive sharpening of DLSS's picture (0 = off;
    // DLSS 4 has none of its own, and Lossless Scaling's NIS does sharpen). estimate: measure the motion from the frames (flow is then
    // ignored). False when the frame could not run; done = doneValue is signalled on the engine's queue either way (after the frames
    // before it), so it only ever moves forward.
    bool Run(ID3D12Resource* in, uint32_t inW, uint32_t inH, DXGI_FORMAT inFormat, ID3D12Resource* out, uint32_t outW, uint32_t outH, DXGI_FORMAT outFormat,
             ID3D12Resource* flow, uint32_t flowW, uint32_t flowH, float flowUnit, float motionFraction, bool estimate, unsigned preset, float sharpen, bool reset,
             bool hdr, ID3D12Fence* copied, uint64_t copiedValue, ID3D12Fence* done, uint64_t doneValue);

    // Stability 0..1 (from the next run): the motion's distrust mask looks past flicker, and FSR 3 keeps more of its history and reacts less
    // to small changes of shading. Less shimmer on thin lines and leaves; more trailing behind what moves. 0 = as before.
    void SetStability(float s) { m_stability.store(s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s); }
    // Edge smoothing 0..1 (from the next run): the upscaler's picture is anti-aliased along its edges before the sharpening (for games
    // without anti-aliasing of their own). 0 = off.
    void SetEdgeSmoothing(float s) { m_edges.store(s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s); }
    // For the offline tools (nr_sreval), before the first run: the motion estimate's cap on straying from its coarser guess
    // (FlowEstimator::SetStrayCap); the motion from which the upscaler leans on the frame (pixels a frame; otherwise kFastMotionShare of the
    // frame's width); the vectors' scale as the upscaler is told it (1: as measured); no distrust mask for the upscaler.
    void SetStrayCap(float pixels) { m_estimator.SetStrayCap(pixels); }
    void SetMeanWeight(float weight) { m_estimator.SetMeanWeight(weight); }
    void SetGradWeight(float weight) { m_estimator.SetGradWeight(weight); }
    void PrepareShapeCost() { m_estimator.PrepareShape(); }   // waits for the shape cost's compile (tools that need it from the first frame)
    void SetFastMotion(float pixels) { m_fastMotion.store(pixels); }
    // What the lean pass blends toward where the picture moves: 0 Catmull-Rom of the frame, 1 FSR 1's edge-adaptive EASU of it.
    void SetLeanMode(uint32_t mode) { m_leanMode.store(mode); }
    // The least share of the plain resample in the picture, even where nothing moves (0..1): the upscalers' own picture at rest is softer than a resample
    // (no camera jitter to find detail with), so this trades some of its anti-aliasing for sharpness at rest.
    void SetLeanRest(float rest) { m_leanRest.store(rest < 0.0f ? 0.0f : rest > 1.0f ? 1.0f : rest); }
    // Sharpening that follows the picture's stability (0..0.95: how much of the running average, 0 off): where the upscaled picture changes from one frame to the next without the motion
    // explaining it (the game's shimmer), the sharpening is reduced by this much, so it puts detail back without amplifying the flicker.
    void SetSteadyMotion(float from, float to) { m_steadyMvA.store(from); m_steadyMvB.store(to); }   // output pixels of motion where the average's weight starts to fall and where it is gone
    void SetFsrOwnSharpen(bool own) { m_fsrOwnSharpen.store(own); }   // FSR's sharpening done by our pass (CAS, with Steady sharpening; the default) instead of AMD's RCAS (softer: 73 to 83 % of the game's detail at 0.5)
    // Every other frame, keep the last motion estimate (2) or only refine its block vectors per pixel (1): for frame generation, which presents two frames for each real one; 0 always estimates in full
    void SetFlowReuse(int mode) { m_flowReuse.store(mode < 0 ? 0 : mode > 2 ? 2 : mode); }
    void SetSteadySign(float sign) { m_steadySign.store(sign); }   // which way along the motion the previous frame is fetched (evaluation)
    void SetSteadySharpen(float steady) { m_steadySharp.store(steady < 0.0f ? 0.0f : steady > 0.95f ? 0.95f : steady); }
    void SetFastMotionShare(float share) { m_fastShare.store(share); }   // where SetFastMotion is automatic (-1): from this share of the width (0: the default)
    void SetMotionScale(float s) { m_motionScale = s; }
    void SetNoMask(bool none) { m_noMask = none; }
    ID3D12Device* Device() const { return m_dev; }   // (the offline tools: the debug layer's messages)
    double AfterMs() const { return m_afterMs; }   // the passes after the upscaler (edges, sharpening), on the GPU, smoothed
    double GpuMs() const { return m_gpuMs; }   // everything a run does, on the GPU, smoothed
    double MotionMs() const { return m_motionMs; }   // of that, the motion (the estimate, or the flow pass)
    uint64_t Runs() const { return m_runs; }
    double LastBuildMs() const { return m_buildMs; }

private:
    struct FfxState;   // AMD's FidelityFX runtime and its upscaling context (sr_engine.cpp)
    struct XessState;  // Intel's XeSS runtime and its context (sr_engine.cpp)
    static const int kSlots = 4;
    bool HasFeature() const;
    const char* Name() const { return m_backend == Backend::Fsr ? "FSR" : m_backend == Backend::Xess ? "XeSS" : "DLSS"; }   // for the log
    bool EnsureFeature(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, unsigned preset, bool hdr);
    bool EnsureViewInput(uint32_t w, uint32_t h);
    bool EnsureInputs(uint32_t w, uint32_t h);
    bool EnsureSharpenTarget(uint32_t w, uint32_t h, DXGI_FORMAT fmt);
    int TakeSlot();
    void ReadTime(int slot);
    bool WaitIdle();
    void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
    void Fail(const char* fmt, ...);
    void Log(const char* fmt, ...);

    LogFn m_log;
    std::atomic<bool> m_ready{ false }, m_failed{ false };
    bool m_abandoned = false;   // the engine's thread did not stop (stuck in a runtime): nothing is torn down or started again until Lossless Scaling restarts
    mutable std::mutex m_errorMutex;   // m_error: written on the engine's thread, read by the panel
    static constexpr DWORD kStuckMs = 20000;   // long enough for a runtime's first shader compile (FSR 4 compiles on its first frame)
    static const int kOkRing = 8;
    std::atomic<uint64_t> m_okRing[kOkRing] = {};   // the done values of jobs that ran
    std::atomic<uint64_t> m_submitted{ 0 };
    std::atomic<ULONGLONG> m_busySince{ 0 };   // when the job in progress started (0: none)
    std::atomic<bool> m_stuck{ false };
    std::thread m_worker; HANDLE m_workerExited = nullptr;
    std::mutex m_jobMutex; std::condition_variable m_jobCv; std::deque<Job> m_jobs; bool m_busy = false, m_stop = false;
    void StartWorker();
    bool StopWorker(DWORD ms);
    bool WaitWorkerIdle(DWORD ms);
    void WorkerLoop();
    std::string m_provider; mutable std::mutex m_providerMutex;   // set on the render thread, read by the panel
    std::string m_error;
    ID3D12Device* m_dev = nullptr;
    ID3D12CommandQueue* m_queue = nullptr;
    ID3D12CommandAllocator* m_alloc[kSlots] = {};
    ID3D12GraphicsCommandList* m_list = nullptr;
    ID3D12Fence* m_fence = nullptr; HANDLE m_event = nullptr; uint64_t m_fenceValue = 0;
    uint64_t m_slotDone[kSlots] = {}; int m_nextSlot = 0;
    ID3D12QueryHeap* m_timestamps = nullptr; ID3D12Resource* m_timestampReadback = nullptr; uint64_t m_timestampFreq = 1;
    double m_gpuMs = 0, m_motionMs = 0;
    FlowEstimator m_estimator; bool m_estimatedLast = false; uint64_t m_estimates = 0; float m_motionScale = 1.0f; bool m_noMask = false; std::atomic<float> m_fastMotion{ -1.0f }, m_fastShare{ 0.0f }; std::atomic<uint32_t> m_leanMode{ 0 }; std::atomic<float> m_leanRest{ 0.0f };
    std::atomic<float> m_stability{ 0.0f }, m_edges{ 0.0f };
    static const int kDescriptors = 17;   // per slot: flow, motion, sharpen in/out, edges in/out, view in/out, lean in (3) / out, steady stabiliser in (3) / out, steady sharpen in (3) / out
    ID3D12PipelineState* m_edgesPso = nullptr;
    // the lean (every upscaler; DLSS takes no mask of its own): its picture blended toward this frame, upscaled plainly, by the distrust mask
    ID3D12RootSignature* m_leanRoot = nullptr; ID3D12PipelineState* m_leanPso = nullptr;
    ID3D12Resource* m_leaned = nullptr; uint32_t m_leanedW = 0, m_leanedH = 0; DXGI_FORMAT m_leanedFmt = DXGI_FORMAT_UNKNOWN;
    bool EnsureLeanTarget(uint32_t w, uint32_t h, DXGI_FORMAT fmt);
    bool InitLean();
    // steady sharpening: the sharpening pass with the previous frame's input and the motion (on the lean's root signature: three pictures in, one out)
    ID3D12RootSignature* m_steadyRoot = nullptr; ID3D12PipelineState* m_steadyPso = nullptr;
    std::atomic<bool> m_fsrOwnSharpen{ true }; std::atomic<int> m_flowReuse{ 0 }; bool m_haveMotion = false; uint32_t m_flowPhase = 0; std::atomic<float> m_steadySharp{ 0.0f }, m_steadySign{ -1.0f }, m_steadyMvA{ 0.5f }, m_steadyMvB{ 3.0f };
    ID3D12Resource* m_sharpHist[2] = {}; int m_sharpHistCur = 0; uint32_t m_sharpHistW = 0, m_sharpHistH = 0; DXGI_FORMAT m_sharpHistFmt = DXGI_FORMAT_UNKNOWN; bool m_sharpHistValid = false;
    bool EnsureSharpHist(uint32_t w, uint32_t h, DXGI_FORMAT fmt);
    ID3D12Resource* m_smoothed = nullptr; uint32_t m_smoothedW = 0, m_smoothedH = 0; DXGI_FORMAT m_smoothedFmt = DXGI_FORMAT_UNKNOWN;
    double m_afterMs = 0;
    bool EnsureSmoothTarget(uint32_t w, uint32_t h, DXGI_FORMAT fmt);
    float m_ffxStability = -1.0f;      // the stability FSR's context was last configured for (-1: not yet)
    float m_loggedStability = -1.0f;
    void ConfigureFsrStability(float s);
    // the motion pass
    ID3D12RootSignature* m_rootSig = nullptr; ID3D12PipelineState* m_motionPso = nullptr; ID3D12PipelineState* m_sharpenPso = nullptr;
    ID3D12Resource* m_unsharpened = nullptr; uint32_t m_unsharpenedW = 0, m_unsharpenedH = 0; DXGI_FORMAT m_unsharpenedFmt = DXGI_FORMAT_UNKNOWN;   // DLSS's picture before sharpening
    ID3D12DescriptorHeap* m_heap = nullptr; uint32_t m_descriptorSize = 0;
    // DLSS
    Backend m_backend = Backend::Dlss;
    FfxState* m_ffx = nullptr;
    XessState* m_xess = nullptr;
    int64_t m_lastRunQpc = 0;   // FSR wants the time between frames
    // NGX's start-up, kept to start it again when its feature is lost under it (another NGX user in the process shut down: RestartNgx)
    std::wstring m_ngxDataPath, m_ngxRuntimeDir; int m_ngxRestarts = 0; bool m_ngxLost = false, m_ngxJoined = false;
    bool RestartNgx();
    void* m_params = nullptr;   // NVSDK_NGX_Parameter*
    void* m_feature = nullptr;  // NVSDK_NGX_Handle*
    uint32_t m_inW = 0, m_inH = 0, m_outW = 0, m_outH = 0; unsigned m_preset = ~0u; bool m_hdr = false;
    ID3D12Resource* m_motion = nullptr;   // RG16F at the game's size, rests readable
    ID3D12Resource* m_distrust = nullptr; // R8 at the game's size, rests readable: where the measured motion cannot be trusted (DLSS's bias mask)
    ID3D12Resource* m_depth = nullptr;    // R32F, flat
    ID3D12Resource* m_view = nullptr; uint32_t m_viewW = 0, m_viewH = 0;   // RGBA16F: an HDR frame's SDR view, for the motion estimate; rests in UNORDERED_ACCESS
    ID3D12PipelineState* m_viewPso = nullptr;
    ID3D12Resource* m_depthUpload = nullptr;
    uint64_t m_runs = 0; double m_buildMs = 0;
};
