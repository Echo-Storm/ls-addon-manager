// OwnSynth: the frames between two frames, made from this project's own motion estimate with our own compute shaders (docs/own-generator-plan.md; the
// method was found with tools/interp_flow_lab.py, method "bi+dir"). No code or idea from Lossless Scaling's frame generation or from any other generator.
//
// Given the frame before (a), this frame (c) and the estimate's vectors (a pixel of c to where it was in a: c(x) ~ a(x + v)), for each time t in (0, 1):
//   1. each cell's match error (how unlike its place in a it is, summed over channels, smoothed over 5x5 cells);
//   2. the vectors are splatted to where their pixels are at time t (x + (1 - t) v); where several land, the best match wins the spot (an atomic minimum on
//      error, then the winner's vector is written): the picture's motion is now defined at the cells of the frame being made;
//   3. the same vectors are splatted onto a's cells (x + v) and reversed: the vectors the other way, without a second estimate; those are splatted to
//      time t as well (a second hypothesis for each cell);
//   4. per cell the hypothesis whose two moved samples agree better (3x3 mean of the disagreement) is taken;
//   5. a cell neither reached takes the vector of the nearest reached cell to its left, right, above or below (or none), whichever makes the moved
//      pair agree best (a hole is background just uncovered: one side's vector is right, the average of both is not);
//   5b. every cell keeps how well its moved pair agrees (its doubt); the frame leans toward the plain mix of the two frames where the doubt is large (`soften`)
//   0. marks (HUD, read-outs): a cell that was alike in both real frames while the picture at large moves around it and would come out clearly another colour
//      drawn the way the picture moves is counted up (by two, to 12), down by one where it goes with the picture; a counted-up cell is held as the plain mix of
//      the two frames, with its neighbours (an anti-aliased letter's edge), where the later frame has it the same (docs/mfg-super-motion-engine-notes.md, step 9;
//      measured with tools/flowlab/hud_lab.py: +10 dB on a HUD over a turning picture, -0.3 dB elsewhere). The counts live from pair to pair.
//   6. the frame, at the frame's own size: each pixel takes, of the 2x2 nearest cells' vectors, the one under which the moved pair agrees best at this
//      pixel, and makes (1 - t) a(q + t F) + t c(q - (1 - t) F).
// Steps 1 to 5 run on cells of `scale` x `scale` pixels (1: every pixel; 2: half size, a quarter of the work and of the memory). For several times t
// (a multiplier) the errors and the vectors onto a (steps 1 and the first half of 3) are made once. Records into the caller's command list (like
// FlowEstimator::Record).
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
    static const int kMaxTargets = 8;   // frames made from one pair of real frames (a multiplier of up to x9)
    struct Target { ID3D12Resource* out; float t; };   // a frame to make: where it goes (UNORDERED_ACCESS) and its time between before (0) and now (1)
    ~OwnSynth() { Shutdown(); }
    bool Init(ID3D12Device* dev, LogFn log);
    void Shutdown();
    bool IsReady() const { return m_ready; }
    // The analysis cells: 1 pixel (the quality reference), 2 (default: a quarter of the work) or 4. Takes effect at the next Ensure.
    void SetAnalysisScale(int scale) { m_wantScale = scale <= 1 ? 1 : scale >= 4 ? 4 : 2; }
    int AnalysisScale() const { return m_scale; }
    // Buffers for frames of this size (the old ones are released at once: the caller has waited for the GPU, or sizes do not change while running).
    bool Ensure(uint32_t w, uint32_t h);
    // before, now: the frames (NON_PIXEL_SHADER_RESOURCE, any format a shader reads as colour, same size); motion: RG16F pixel vectors c to a
    // (NON_PIXEL_SHADER_RESOURCE); each target's out: UNORDERED_ACCESS, a format that takes typed stores, same size. encoding, whiteNits: what the frames hold
    // (hdr_hlsl.h: 0 SDR, 1 scRGB, 2 HDR10) and the SDR white; HDR frames are compared and blended in their SDR view and the frames made go back into their
    // encoding. Leaves the list's descriptor heap and root signature changed.
    bool Record(ID3D12GraphicsCommandList* list, int slot, ID3D12Resource* before, DXGI_FORMAT beforeFormat, ID3D12Resource* now, DXGI_FORMAT nowFormat,
                ID3D12Resource* motion, DXGI_FORMAT outFormat, const Target* targets, int count, uint32_t encoding = 0, float whiteNits = 80.0f, float soften = 0.6f, float marks = 1.0f,
                ID3D12Resource* motionReverse = nullptr);
    // motionReverse (RG16F pixel vectors a to c, NON_PIXEL_SHADER_RESOURCE; the estimate run on the pair the other way round): the direct draw. Each pixel of the frame made takes the
    // movement found AT that pixel from both fields (where they agree, their mean; else the one under which the moved pair agrees better) and is drawn straight from the two real
    // frames, nothing carried: no splats, hypotheses or holes (docs/mfg-super-motion-engine-notes.md, step 11; board E18). Without it, the splat synthesis above.
    // marks 0..1: whether the marks (see above) are learnt and held (NR_SYNTH_MARKS=0 in the environment turns them off for comparing)
    // soften 0..1: how far a frame leans toward the plain mix of the two real frames where its motion is doubtful (0: never; 1: up to 80 % where it is clearly wrong)
    // With a timestamp frequency (the queue's) the passes are timed on the GPU: ReadMs(slot) after the slot's work has finished.
    void SetTimestampFrequency(uint64_t f) { m_freq = f; }
    bool ReadMs(int slot, double& ms);

private:
    enum Entry { Clear, Err0, Box5, ErrA, Splat, Hyp1, Hyp2, Holes, Synth, MarkPrep, MarkLarge, MarkUpdate, Direct, EntryCount };
    // (the direct draw uses only MK among them)
    // buffers (one uint a cell): E0, Ea0 (errors, then the hypotheses' disagreement), K1, K2, K3 (splat keys), V1, V2, V3 (their vectors), B, B2 (the 5x5 means),
    // FL (which hypothesis a cell took), MK (the marks' counts, kept from pair to pair)
    static const int kBuffers = 12, kSrvs = 4, kUavs = kBuffers + 1, kPerSlot = kSrvs + kMaxTargets * kUavs;
    void Log(const char* msg) { if (m_log) m_log(msg); }
    LogFn m_log;
    ID3D12Device* m_dev = nullptr; ID3D12RootSignature* m_root = nullptr; ID3D12PipelineState* m_pso[EntryCount] = {};
    ID3D12DescriptorHeap* m_heap = nullptr; uint32_t m_descriptorSize = 0;
    ID3D12Resource* m_buf[kBuffers] = {};
    uint32_t m_w = 0, m_h = 0, m_aw = 0, m_ah = 0; int m_scale = 2, m_wantScale = 2; bool m_ready = false;
    ID3D12QueryHeap* m_stamps = nullptr; ID3D12Resource* m_stampReadback = nullptr; uint64_t m_freq = 0;
};

} // namespace nr
