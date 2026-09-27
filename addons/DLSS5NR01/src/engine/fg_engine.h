// FgEngine: frame generation of our own on a Direct3D 12 device of its own (docs/frame-generation-research.md). Lossless Scaling's
// presented frame comes in through a shared texture; this project's motion estimate measures its motion; AMD's FSR 3.1 frame generation
// makes the frame between it and the one before, at present time on a FidelityFX swap chain of its own (a window never shown); the
// generated frame goes out through a second shared texture. Everything runs on the caller's thread (Lossless Scaling's presenting thread,
// through framegen11), which Generate holds until the frame is made.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>
#include <functional>
#include <string>
#include "engine/flow_estimator.h"

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
    // Frame n is in `in` once `copied` reaches n. Makes the frame between frame n - 1 and n into `out` and returns true once it is written
    // there (false: no frame between this time, such as for the first frame; `out` is then untouched). frameMs: the time since frame n - 1.
    // encoding, whiteNits: what the frames hold (hdr_hlsl.h: 0 SDR, 1 scRGB, 2 HDR10) and the SDR white; the motion is measured in their SDR view.
    bool Generate(ID3D12Resource* in, ID3D12Fence* copied, uint64_t n, ID3D12Resource* out, float frameMs, bool reset, uint32_t encoding = 0, float whiteNits = 80.0f);
    const char* LastError() const { return m_error.c_str(); }

private:
    struct Ffx;
    void Log(const char* fmt, ...);
    bool Fail(const char* what);
    void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
    bool Submit(DWORD waitMs = 5000);   // runs the list and waits for it

    LogFn m_log; std::string m_error; bool m_ready = false;
    LUID m_card{}; uint32_t m_w = 0, m_h = 0; DXGI_FORMAT m_fmt = DXGI_FORMAT_UNKNOWN; bool m_hdr = false;
    ID3D12Device* m_dev = nullptr; ID3D12CommandQueue* m_queue = nullptr; ID3D12CommandAllocator* m_alloc = nullptr; ID3D12GraphicsCommandList* m_list = nullptr;
    ID3D12Fence* m_fence = nullptr; uint64_t m_fenceValue = 0; HANDLE m_event = nullptr;
    FlowEstimator m_estimator;
    ID3D12Resource* m_motion = nullptr; ID3D12Resource* m_distrust = nullptr; ID3D12Resource* m_depth = nullptr;
    ID3D12Resource* m_marker = nullptr;   // readback: a few texels of the generated frame, copied after it (their arrival says it is written)
    HWND m_hwnd = nullptr; IDXGISwapChain4* m_chain = nullptr;
    Ffx* m_ffx = nullptr;
    uint64_t m_frameId = 0;
};

} // namespace nr
