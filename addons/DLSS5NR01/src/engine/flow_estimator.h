// FlowEstimator: motion vectors measured from the frames themselves, for the DLSS Upscaler (and any later upscaler) when the game gives
// none. It runs on the engine's own D3D12 device, inside the engine's command list, before DLSS.
//
// Each frame: its brightness at the game's size and at halved sizes down to about 64 pixels wide (a pyramid, kept for the next frame). Then,
// from the smallest size up to half size, every 4x4 block finds where its 8x8 neighbourhood was in the frame before: the guess from the size
// below (and its neighbours) is tried first, then the positions around the best of them, with a small cost for straying from the guess so
// flat areas follow their surroundings instead of noise. At half size the answer is refined to a fraction of a pixel. A 3x3 vector median
// removes the odd wrong block. Last, at the game's size, every pixel picks, from its own block's vector, the three nearest blocks' vectors
// and "not moving", the one under which its 3x3 surroundings match best, so motion follows object edges and a HUD that stays put stays put.
//
// The vectors point from a pixel in this frame to where it was in the frame before, in pixels of the game's frame (DLSS's convention with
// its low-resolution motion flag). Without a frame before (the first, or after a size change) they are zero.
//
// Beside the vectors, a mask of where they cannot be trusted: where even the best vector leaves the pixel's surroundings unlike the frame
// before (background just uncovered, transparent effects, a wrong estimate). DLSS takes it as its "bias toward the current frame" mask and
// leans on this frame there instead of smearing its history.
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <functional>
#include <vector>

class FlowEstimator {
public:
    using LogFn = std::function<void(const char*)>;
    static const int kSlots = 4;           // the engine's command allocator slots (statistics are read back per slot)
    bool Init(ID3D12Device* dev, LogFn log);
    void Shutdown();
    bool IsReady() const { return m_pso[0] != nullptr; }

    bool NeedsResize(uint32_t w, uint32_t h) const { return w != m_w || h != m_h; }
    // Makes the pyramids and grids for frames of this size. retireAt 0: the GPU is not using the old ones (the caller waited). Otherwise the
    // old textures are kept until the caller's fence passes retireAt (Collect), so the frame path never waits for a size change.
    bool Ensure(uint32_t w, uint32_t h, uint64_t retireAt = 0);
    void Collect(uint64_t completed);   // frees retired textures whose work the GPU has finished
    void Forget() { m_havePrevious = false; }   // the next frame has no frame before (a cut)

    // Records the passes. frame: the game's frame (readable, NON_PIXEL_SHADER_RESOURCE; RGBA8); motion: RG16F at the frame's size, in
    // UNORDERED_ACCESS, receives the vectors; distrust: R8 at the frame's size, in UNORDERED_ACCESS, receives the mask (0 trusted .. 1 not).
    // Leaves the command list's descriptor heap and root signature changed.
    // stability 0..1: how far the distrust mask looks past flicker (see the pixel pass). encoding: the frame's (hdr_hlsl.h: 0 SDR, 1 scRGB,
    // 2 HDR10), matched in its SDR view with the SDR white whiteNits (the upscaler hands its SDR view in already: 0).
    void Record(ID3D12GraphicsCommandList* list, int slot, ID3D12Resource* frame, DXGI_FORMAT frameFormat, ID3D12Resource* motion, ID3D12Resource* distrust,
                float stability = 0.0f, uint32_t encoding = 0, float whiteNits = 80.0f);
    // After the engine has reused the slot (its earlier work is finished): that frame's statistics join the running totals.
    void ReadStats(int slot);
    // The average vector (game pixels), match cost (0..1 per pixel) and distrust (0..1) since the last call; false when no frame was measured.
    bool TakeAverages(double& x, double& y, double& length, double& cost, double& distrust, uint64_t& frames);
    // The average vector length (the frame's pixels) of the last frame whose statistics were read (ReadStats); 0 before any.
    double LastLength() const { return m_lastLength; }
    int Levels() const { return m_levels; }
    // How far (pixels of each size) straying from the coarser size's guess costs more (the search pass): 2 by default; very large leaves it
    // uncapped, as it was until 2026-09-27 (for comparing).
    void SetStrayCap(float pixels) { m_strayCap = pixels; }
    // From how fast (pixels a frame, the frame's own) the distrust mask rises, fully at twice that: the upscaler leans on the current frame in
    // fast motion. 0: off.
    void SetFastMotion(float pixels) { m_fastMotion = pixels; }
    // how much the blocks' difference in average brightness counts in a match (1: all of it, the plain difference; below: the shape counts more)
    void SetMeanWeight(float weight) { m_meanWeight = weight; }
    void SetTimestampFrequency(uint64_t f) { m_timestampFreq = f; }
    // The GPU time of each stage (the pyramid, the search, the median, every pixel), averaged since the last call; false without any.
    bool TakeStageTimes(double ms[4]);

private:
    static const int kMaxLevels = 7, kDescriptorsPerPass = 6, kPassesMax = 3 + 2 * kMaxLevels, kDescriptorsPerSlot = kDescriptorsPerPass * kPassesMax;
    enum Pso { Luma, Down, Global, Search, Median, Pixel, PsoCount };
    static const int kGlobalRadius = 20;          // the whole picture's shift, tried within +-this at the smallest size (+-640 pixels at 4K)
    struct Pass { D3D12_GPU_DESCRIPTOR_HANDLE srvs, uav; };
    Pass MakePass(int slot, int& index, ID3D12Resource* const srv[4], const DXGI_FORMAT srvFormat[4], ID3D12Resource* uav, DXGI_FORMAT uavFormat,
                  ID3D12Resource* uav2 = nullptr, DXGI_FORMAT uav2Format = DXGI_FORMAT_R8_UNORM);
    void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
    void Release(uint64_t retireAt = 0);
    struct Retired { ID3D12Resource* texture; uint64_t at; };
    std::vector<Retired> m_retired;
    void Log(const char* fmt, ...);

    LogFn m_log;
    ID3D12Device* m_dev = nullptr;
    ID3D12RootSignature* m_root = nullptr;
    ID3D12PipelineState* m_pso[PsoCount] = {};
    ID3D12DescriptorHeap* m_heap = nullptr; uint32_t m_descriptorSize = 0;
    uint32_t m_w = 0, m_h = 0; int m_levels = 0;
    uint32_t m_lw[kMaxLevels] = {}, m_lh[kMaxLevels] = {}, m_gw[kMaxLevels] = {}, m_gh[kMaxLevels] = {};
    ID3D12Resource* m_luma[2][kMaxLevels] = {};   // two pyramids: this frame's and the one before's (R16F, rest readable)
    ID3D12Resource* m_grid[kMaxLevels] = {};      // one vector per 4x4 block of each size from 1 up (RG16F, in that size's pixels, rest readable)
    ID3D12Resource* m_filtered = nullptr;         // the half-size grid after the median
    ID3D12Resource* m_global = nullptr;           // the cost of each shift of the whole picture (R32F, (2r+1) squared)
    ID3D12Resource* m_stats = nullptr;            // 8 uints: summed match cost, x and y (1/16 pixel), blocks, length; summed distrust (1/100), pixels
    ID3D12Resource* m_statsReadback = nullptr;    // kSlots x 16 bytes
    bool m_statsPending[kSlots] = {};
    static const int kStamps = 5;                 // start, pyramid done, search done, median done, pixels done
    ID3D12QueryHeap* m_stamps = nullptr; ID3D12Resource* m_stampReadback = nullptr; uint64_t m_timestampFreq = 0;
    bool m_stampsPending[kSlots] = {};
    double m_stageSum[4] = {}; uint64_t m_stageCount = 0;
    int m_current = 0; bool m_havePrevious = false; float m_strayCap = 2.0f; float m_fastMotion = 0.0f; float m_meanWeight = 1.0f;
    // running totals for TakeAverages
    double m_sumCost = 0, m_sumX = 0, m_sumY = 0, m_sumLength = 0, m_blocks = 0, m_sumDistrust = 0, m_pixels = 0; uint64_t m_frames = 0;
    double m_lastLength = 0;   // the average vector length of the frame read last (LastLength)
};
