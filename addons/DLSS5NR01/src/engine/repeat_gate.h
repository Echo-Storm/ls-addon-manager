// RepeatGate: is this picture the very one before? (docs/repeat-skip-spec.md)
//
// An exact compare of two pictures on the engine's own Direct3D 12 queue, and the copy that lets a repeated picture show the last output again instead of being worked on.
// It runs on the engine's worker thread, never on Lossless Scaling's render thread, so it may wait for its answer (a fence and a small read back): the engine's pictures are
// already shown a frame or two after they are handed over.
//
//   Compare   the input against the one it kept from the call before (every pixel, every colour channel, exactly, or within `tolerance` of the channel's range), and keeps
//             this input for the next call. A first picture, a new size or format, or a failure answers Different (never a wrong Same).
//   CopyPicture  a GPU copy of one shared picture into another of the same size and format, on the same queue (so it follows the work before it in order).
#pragma once
#include <d3d12.h>
#include <cstdint>

namespace nr {

class RepeatGate {
public:
    enum class Result { Different, Same };
    ~RepeatGate() { Shutdown(); }
    bool Init(ID3D12Device* dev, ID3D12CommandQueue* queue);
    void Shutdown();
    bool Ready() const { return m_ready; }
    // `in` is in COMMON on entry and on return (a shared texture). The queue waits for `copied` to reach `copiedValue` before it reads `in`. `viewFormat` is how the shader reads it.
    // The answer is waited for at most `waitMs` (a busy card may be slow to get to it): later than that it is Different and the picture is simply worked on, as without the gate.
    Result Compare(ID3D12Resource* in, DXGI_FORMAT viewFormat, ID3D12Fence* copied, uint64_t copiedValue, float tolerance, unsigned waitMs = 4);
    // src and dst are in COMMON on entry and on return.
    bool CopyPicture(ID3D12Resource* src, ID3D12Resource* dst);
    // Forget the kept input: the next Compare answers Different (a changed setting, a new session).
    void Forget() { m_havePrev = false; }
    double LastCompareMs() const { return m_lastMs; }   // the last compare's wait, in milliseconds
    uint64_t TimedOut() const { return m_timedOut; }

private:
    void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
    bool EnsurePrev(const D3D12_RESOURCE_DESC& desc);
    bool WaitFor(uint64_t value, unsigned ms);   // until the fence has reached `value` (a signal left over from an earlier, timed out wait does not count)
    ID3D12Device* m_dev = nullptr; ID3D12CommandQueue* m_queue = nullptr; bool m_ready = false;
    ID3D12RootSignature* m_root = nullptr; ID3D12PipelineState* m_pso = nullptr; ID3D12DescriptorHeap* m_heap = nullptr; UINT m_descriptorSize = 0;
    ID3D12Resource* m_flag = nullptr; ID3D12Resource* m_zero = nullptr; ID3D12Resource* m_readback = nullptr;
    ID3D12CommandAllocator* m_alloc = nullptr; ID3D12GraphicsCommandList* m_list = nullptr;
    ID3D12Fence* m_fence = nullptr; void* m_event = nullptr; uint64_t m_fenceValue = 0;
    static constexpr int kCopyRing = 4;
    ID3D12CommandAllocator* m_copyAlloc[kCopyRing] = {}; uint64_t m_copyDone[kCopyRing] = {}; int m_copyNext = 0;   // the copies run without being waited for: their allocators come round
    ID3D12Resource* m_prev = nullptr; D3D12_RESOURCE_DESC m_prevDesc{}; bool m_havePrev = false;   // the input kept, at rest in COPY_DEST
    double m_lastMs = 0; uint64_t m_timedOut = 0;
};

} // namespace nr
