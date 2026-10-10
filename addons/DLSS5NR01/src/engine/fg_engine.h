// FgEngine: frame generation of our own on a Direct3D 12 device of its own (docs/frame-generation-research.md). Lossless Scaling's
// presented frame comes in through a shared texture; this project's motion estimate measures its motion; AMD's FSR 3.1 frame generation
// makes the frame between it and the one before, dispatched straight on our command list (no FidelityFX swap chain: its presents paced
// themselves on the frame rate and held Lossless Scaling's thread about 20 ms a frame); the frame made goes out through a second shared
// texture and a shared fence says when. Nothing here waits for the GPU on the caller's thread (Lossless Scaling's presenting thread).
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>
#include <functional>
#include <string>
#include "engine/flow_estimator.h"
#include "engine/own_synth.h"

namespace nr {

class FgEngine {
public:
    using LogFn = std::function<void(const char*)>;
    ~FgEngine() { Shutdown(); }
    // On the card with this LUID; runtimeDll: AMD's amd_fidelityfx_dx12.dll (the FSR Upscaler's). Frames of w x h in format fmt (the swap
    // chain's: 8-bit SDR, 10-bit or half-float HDR); hdr: the frames are HDR (FSR's HDR mode).
    bool Init(const LUID& card, const std::wstring& runtimeDll, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool hdr, LogFn log);
    void Shutdown();
    bool IsReady() const { return m_ready; }
    bool Matches(const LUID& card, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool hdr) const {
        return m_ready && card.LowPart == m_card.LowPart && card.HighPart == m_card.HighPart && w == m_w && h == m_h && fmt == m_fmt && hdr == m_hdr;
    }
    ID3D12Resource* OpenSharedTexture(HANDLE h);
    ID3D12Fence* OpenSharedFence(HANDLE h);
    // Frame n is in `in` once `copied` reaches n. Queues the making of the frame between frame n - 1 and n into `out` (COMMON, shared) and
    // the signal made = madeValue after it, and returns at once: true when a frame between is on its way (wait for `made` on the GPU before
    // reading `out`); false when there is none this time (the first frame, a reset, an error: `made` is still signalled, `out` untouched).
    // frameMs: the time since frame n - 1. encoding, whiteNits: what the frames hold (hdr_hlsl.h: 0 SDR, 1 scRGB, 2 HDR10) and the SDR white;
    // the motion is measured in their SDR view.
    // guard: the guard against pasted background (where the two real frames agree around a pixel the motion calls slow, and the frame made
    // is far from both, the real frames' mix goes there: a character the camera follows, pasted over by the scene sweeping past in a turn).
    bool Generate(ID3D12Resource* in, ID3D12Fence* copied, uint64_t n, ID3D12Resource* out, ID3D12Fence* made, uint64_t madeValue, float frameMs, bool reset,
                  uint32_t encoding = 0, float whiteNits = 80.0f, bool guard = true) {
        return GenerateFrames(in, copied, n, &out, 1, made, madeValue, frameMs, reset, encoding, whiteNits, guard) > 0;
    }
    // The same for a multiplier: up to `count` frames between frame n - 1 and n (at k / (made + 1), k = 1 .. made) into outs[0 .. made - 1]; returns how many were made
    // (0: none; 1 when count is more but our own synthesis is not in use: FSR makes the one at half way). The `made` fence is signalled once, after all of them.
    int GenerateFrames(ID3D12Resource* in, ID3D12Fence* copied, uint64_t n, ID3D12Resource* const* outs, int count, ID3D12Fence* made, uint64_t madeValue, float frameMs, bool reset,
                       uint32_t encoding = 0, float whiteNits = 80.0f, bool guard = true);
    // How many frames between the engine can make at once: more than 1 only with our own synthesis ready (after the first frame it was asked for).
    int MaxFramesBetween() const { return m_own && m_synthReady ? OwnSynth::kMaxTargets : 1; }
    // The analysis size of our own synthesis (1: every pixel, 2: half size; 2 by default); at the next start of it.
    void SetAnalysisScale(int scale) { m_synth.SetAnalysisScale(scale); }
    // true: the frame between is made by this project's own synthesis (own_synth.h) instead of FSR 3.1; when it cannot start on this card (a frame format
    // without typed stores) FSR makes the frame as before. Set from the thread that calls Generate.
    void SetOwnSynthesis(bool on) { m_own = on; }
    bool UsingOwnSynthesis() const { return m_own && m_synthReady; }
    // How long the GPU took for a frame between (the motion, FSR, the copy out), smoothed over the frames it has finished; 0 before any.
    double GpuMs() const { return m_gpuMs; }
    const char* LastError() const { return m_error.c_str(); }

private:
    struct Ffx;
    static const int kSlots = FlowEstimator::kSlots;   // command allocators in flight (the estimate reads its statistics back per slot)
    void Log(const char* fmt, ...);
    bool Fail(const char* what);
    void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
    bool WaitFence(uint64_t value, DWORD ms);   // (setting up, a slot still busy, shutting down)
    void ReadSlot(int slot);                    // a slot the GPU has finished: its timing and motion statistics

    LogFn m_log; std::string m_error; bool m_ready = false;
    LUID m_card{}; uint32_t m_w = 0, m_h = 0; DXGI_FORMAT m_fmt = DXGI_FORMAT_UNKNOWN; bool m_hdr = false;
    ID3D12Device* m_dev = nullptr; ID3D12CommandQueue* m_queue = nullptr; ID3D12GraphicsCommandList* m_list = nullptr;
    ID3D12CommandAllocator* m_alloc[kSlots] = {}; uint64_t m_slotValue[kSlots] = {}; bool m_slotRead[kSlots] = {};
    ID3D12Fence* m_fence = nullptr; uint64_t m_fenceValue = 0; HANDLE m_event = nullptr;
    ID3D12QueryHeap* m_stamps = nullptr; ID3D12Resource* m_stampReadback = nullptr; uint64_t m_stampFreq = 0; double m_gpuMs = 0;
    FlowEstimator m_estimator;
    ID3D12Resource* m_motion = nullptr; ID3D12Resource* m_distrust = nullptr; ID3D12Resource* m_depth = nullptr;
    ID3D12Resource* m_made = nullptr;   // FSR's output (unordered access, the frames' format), copied into the caller's shared texture
    ID3D12Resource* m_madeMore[OwnSynth::kMaxTargets] = {};   // our own synthesis's frames (the first is m_made), made when a multiplier asks for them
    // the guard: its pass, the frame before (kept for it) and its output
    ID3D12RootSignature* m_guardRoot = nullptr; ID3D12PipelineState* m_guardPso = nullptr; ID3D12DescriptorHeap* m_guardHeap = nullptr; UINT m_descSize = 0;
    ID3D12Resource* m_before = nullptr; ID3D12Resource* m_guarded = nullptr; bool m_haveBefore = false;
    bool InitGuard();
    OwnSynth m_synth; bool m_own = false, m_synthTried = false, m_synthReady = false;
    Ffx* m_ffx = nullptr;
    uint64_t m_frameId = 0, m_runs = 0;
};

} // namespace nr
