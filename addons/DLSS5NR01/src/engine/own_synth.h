// OwnSynth: the frame between two frames, made from this project's own motion estimate with our own compute shaders (docs/own-generator-plan.md; the
// method was found with tools/interp_flow_lab.py, method "bi+dir"). No code or idea from Lossless Scaling's frame generation or from any other generator.
//
// Given the frame before (a), this frame (c) and the estimate's vectors (a pixel of c to where it was in a: c(x) ~ a(x + v)), for a time t in (0, 1):
//   1. each pixel's match error (how unlike its place in a it is, summed over channels, smoothed over 5x5);
//   2. the vectors are splatted to where their pixels are at time t (x + (1 - t) v); where several land, the best match wins the spot (an atomic minimum on
//      error, then the winner's vector is written): the picture's motion is now defined at the pixels of the frame being made;
//   3. the same vectors are splatted onto a's pixels (x + v) and reversed: the vectors the other way, without a second estimate; those are splatted to
//      time t as well (a second hypothesis for each output pixel);
//   4. per output pixel the hypothesis whose two moved samples agree better (3x3 mean of the disagreement) is taken; where both exist, the better;
//   5. a pixel neither reached takes the vector of the nearest reached pixel to its left, right, above or below (or none), whichever makes the moved
//      pair agree best over 5x5 (a hole is background just uncovered: one side's vector is right, the average of both is not);
//   6. the frame: (1 - t) a(q + t F) + t c(q - (1 - t) F), F the vector of that pixel.
// Records into the caller's command list (like FlowEstimator::Record); buffers of its own (8 x 4 bytes a pixel).
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <functional>

namespace nr {

class OwnSynth {
public:
    using LogFn = std::function<void(const char*)>;
    static const int kSlots = 4;
    ~OwnSynth() { Shutdown(); }
    bool Init(ID3D12Device* dev, LogFn log);
    void Shutdown();
    bool IsReady() const { return m_ready; }
    // Buffers for frames of this size (the old ones are released at once: the caller has waited for the GPU, or sizes do not change while running).
    bool Ensure(uint32_t w, uint32_t h);
    // before, now: the frames (NON_PIXEL_SHADER_RESOURCE, any format a shader reads as colour, same size); motion: RG16F pixel vectors c to a
    // (NON_PIXEL_SHADER_RESOURCE); out: the frame made (UNORDERED_ACCESS, a format that takes typed stores, same size). t: the time of the frame made
    // between them (0 = before, 1 = now). Leaves the list's descriptor heap and root signature changed.
    // With a timestamp frequency (the queue's) the passes are timed on the GPU: ReadMs(slot) after the slot's work has finished.
    void SetTimestampFrequency(uint64_t f) { m_freq = f; }
    bool ReadMs(int slot, double& ms);
    bool Record(ID3D12GraphicsCommandList* list, int slot, ID3D12Resource* before, DXGI_FORMAT beforeFormat, ID3D12Resource* now, DXGI_FORMAT nowFormat,
                ID3D12Resource* motion, ID3D12Resource* out, DXGI_FORMAT outFormat, float t);

private:
    enum Entry { Clear, Err0, ErrA, Splat, Hyp1, Hyp2, Synth, EntryCount };
    static const int kBuffers = 8, kSrvs = 3, kUavs = kBuffers + 1, kPerSlot = kSrvs + kUavs;
    void Log(const char* msg) { if (m_log) m_log(msg); }
    LogFn m_log;
    ID3D12Device* m_dev = nullptr; ID3D12RootSignature* m_root = nullptr; ID3D12PipelineState* m_pso[EntryCount] = {};
    ID3D12DescriptorHeap* m_heap = nullptr; uint32_t m_descriptorSize = 0;
    ID3D12Resource* m_buf[kBuffers] = {};   // E0, Ea0, K1, K2, K3, V1, V2, V3
    uint32_t m_w = 0, m_h = 0; bool m_ready = false;
    ID3D12QueryHeap* m_stamps = nullptr; ID3D12Resource* m_stampReadback = nullptr; uint64_t m_freq = 0;
};

} // namespace nr
