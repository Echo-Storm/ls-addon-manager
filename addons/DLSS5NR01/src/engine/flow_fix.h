// FlowFix: the picture at large, as a pass after the motion estimate (docs/mfg-super-motion-engine-notes.md, step 8; board E16).
//
// The estimate reads two things wrongly: a block of fine repeating lines (a fence, a tower's face: it matches as well at no motion, or one line on, as where it truly went) and a block
// that shows nothing (a gradient, a soft wall: it matches anywhere, so it stays "still"). Both then come out of the frame generator as ghosts or a patchwork where the picture around
// them is right. This pass finds how the picture around moves and gives it to the pixels that cannot tell:
//   1. brightness at half size for both frames;
//   2. per tile (and the tiles around it) the dominant movement: the mode of the trusted, moving pixels' vectors (2 px bins, then the mean of those near the peak), and the share of the
//      window's trusted moving pixels that agree with it;
//   3. per half-size cell: the cell's own vector is replaced by its tile's where (a) the cell is plain, or a patch like it matches about as well 6 to 16 pixels along or across (repeating
//      lines), (b) the cell's own vector matches well, (c) the dominant vector carries the cell about as well (within 10 % and 1.5 levels), (d) the own vector is not faster than the picture's
//      (a faster one is a thing of its own: a pole, a character) and the two differ by over a pixel; or (e) the dominant vector carries the cell clearly better (8 levels);
//   4. the result at the frames' size, in a texture of its own (the estimate's stays as it was: the upscalers do not use this).
// Measured (tools/flowlab, nr_fgeval mvfile=): on a made-up scene with those cases the frames made by EchoFG went from +9.94 to +13.68 dB over a plain blend (the repeating block from
// -2.2 to +6.5 dB, the pole kept); on three real recordings it changes 15-32 % of the pixels and the score by +0.00 to +0.02 dB.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <functional>

namespace nr {

class FlowFix {
public:
    using LogFn = std::function<void(const char*)>;
    static const int kSlots = 4;
    ~FlowFix() { Shutdown(); }
    bool Init(ID3D12Device* dev, LogFn log);
    void Shutdown();
    bool IsReady() const { return m_ready; }
    // Textures for frames of this size (the old ones are released at once: the caller has waited for the GPU, or sizes do not change while running).
    bool Ensure(uint32_t w, uint32_t h);
    // before, now: the two frames (NON_PIXEL_SHADER_RESOURCE, any format a shader reads as colour, same size); motion: the estimate (RG16F, pixel vectors now -> before,
    // NON_PIXEL_SHADER_RESOURCE); distrust: its mask (R8, same state); out: the corrected vectors (RG16F, UNORDERED_ACCESS before and after). encoding / whiteNits: as OwnSynth::Record.
    // Leaves the list's descriptor heap and root signature changed.
    bool Record(ID3D12GraphicsCommandList* list, int slot, ID3D12Resource* before, DXGI_FORMAT beforeFormat, ID3D12Resource* now, DXGI_FORMAT nowFormat,
                ID3D12Resource* motion, ID3D12Resource* distrust, ID3D12Resource* out, uint32_t encoding = 0, float whiteNits = 80.0f);
    // With a timestamp frequency (the queue's) the passes are timed on the GPU: ReadMs(slot) after the slot's work has finished.
    void SetTimestampFrequency(uint64_t f) { m_freq = f; }
    bool ReadMs(int slot, double& ms);
    // (The share of cells taken over by the picture's vector is not read back: it would stall; tools count with nr_fgeval's scores.)

private:
    enum Entry { Luma, TileMode, Decide, Apply, EntryCount };
    static const int kSrvs = 8, kUavs = 5, kPerSlot = kSrvs + kUavs;
    void Log(const char* msg) { if (m_log) m_log(msg); }
    void Release();
    LogFn m_log;
    ID3D12Device* m_dev = nullptr; ID3D12RootSignature* m_root = nullptr; ID3D12PipelineState* m_pso[EntryCount] = {};
    ID3D12DescriptorHeap* m_heap = nullptr; uint32_t m_descriptorSize = 0;
    ID3D12Resource *m_lumaA = nullptr, *m_lumaC = nullptr, *m_modes = nullptr, *m_cells = nullptr;   // half-size brightness of both frames; per tile: the mode; per cell (2 x 2 pixels, or 4 x 4 from 1920 wide): the vector to use
    uint32_t m_w = 0, m_h = 0, m_cw = 0, m_ch = 0, m_cs = 2, m_tile = 32, m_tx = 0, m_ty = 0;
    bool m_ready = false;
    ID3D12QueryHeap* m_stamps = nullptr; ID3D12Resource* m_stampReadback = nullptr; uint64_t m_freq = 0;
};

} // namespace nr
