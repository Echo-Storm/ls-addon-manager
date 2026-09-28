// nr_fgeval: how well a frame generator rebuilds frames of a real recording, offline (no Lossless Scaling, no game).
//
// It reads a .lsrec recording (the recorder's; made with frame generation off, so every frame is a real one), keeps every other frame and
// rebuilds each dropped one from the two kept around it, then scores the rebuilt frame against the real one. Scored beside it, two
// baselines: the frame before shown again ("hold") and the two kept frames mixed half and half ("blend"). The worst frames are written
// as pictures (the real frame | the generator's | the blend) so the failures can be seen.
//
// Two generators, each fed this project's motion estimate (src/engine/flow_estimator.cpp) with a flat depth, as the upscalers are:
// AMD's FSR 3.1 frame generation (gen=fsr, the default; the FidelityFX runtime the FSR Upscaler ships) and Intel's XeSS frame generation
// (gen=xess; libxess_fg.dll from Intel's SDK, toolsetch_xess_sdk.ps1). Both run at present time on a swap chain of their own, as in a game.
// Frames are compared in their SDR view: HDR recordings are rolled off as screenshots show them.
//
//   nr_fgeval <recording.lsrec> <output folder> [gen=fsr|xess] [first=N] [count=N] [worst=N] [fsr=<amd_fidelityfx_dx12.dll>] [xessfg=<libxess_fg.dll>]
//             [live=1] [mvcheck=1] [mvscale=N] [depth=motion|noise|layer|orbit [depthpx=N] [layern=N]] [estin=light|view] [depthdir=<folder> [depthinv=0]]
//             [motion=none] [refine=...] [keepstill=N] [straycap=N] [worstby=band] [mvdump=1] [debugview=1] [ffxdebug=1]
// Besides the whole picture, a "band" score: around what moves unlike the camera (a character it follows, and a margin), where turn ghosting is.
// Scores come in full and "coarse" (a quarter of the size: where things are, not their fine detail). In dense foliage the full score cannot
// tell a sharp frame a little off from a double image; look at the pictures (worst=, live=1) before trusting a fraction of a dB.
#include <windows.h>
#include <shlobj.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "addon/lsrec.h"
#include "eval_common.h"
#include "engine/flow_estimator.h"
#include "engine/fg_engine.h"
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_framegeneration.h"
#include "ffx_api/ffx_api_loader.h"
#include "ffx_api/dx12/ffx_api_dx12.h"
#include <functional>
#include <type_traits>
#if __has_include("xess_fg/xefg_swapchain_d3d12.h")
#define NR_HAVE_XEFG 1
#include "xess_fg/xefg_swapchain_d3d12.h"
#include "xell/xell_d3d12.h"
#endif

namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

using namespace nr::eval;

// What stays put between two real frames stays put in the frame between: where a and b (RGBA8) are alike, the picture becomes their mix.
// Alike: the largest channel difference, averaged over the pixel's 3x3, at most `still` levels (all mix), fading to the picture's own by
// twice that. A third-person character carried by a turning camera, a HUD, subtitles: the generator pastes the moving background over them.
void KeepStill(std::vector<uint8_t>& picture, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t w, uint32_t h, float still) {
    std::vector<float> d(static_cast<size_t>(w) * h);
    for (size_t k = 0; k < d.size(); ++k) {
        int m = 0; for (int c = 0; c < 3; ++c) m = std::max(m, std::abs(int(a[k * 4 + c]) - int(b[k * 4 + c])));
        d[k] = static_cast<float>(m);
    }
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
        float s = 0; int n = 0;
        for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
            const int xx = int(x) + i, yy = int(y) + j;
            if (xx >= 0 && yy >= 0 && xx < int(w) && yy < int(h)) { s += d[static_cast<size_t>(yy) * w + xx]; ++n; }
        }
        const float t = std::clamp((s / n - still) / still, 0.0f, 1.0f);   // 0: still (the mix), 1: the generator's
        if (t >= 1.0f) continue;
        const size_t k = (static_cast<size_t>(y) * w + x) * 4;
        for (int c = 0; c < 3; ++c) picture[k + c] = static_cast<uint8_t>(std::lround((a[k + c] + b[k + c]) * 0.5f * (1.0f - t) + picture[k + c] * t));
    }
}

struct Gpu {
    ID3D12Device* dev = nullptr; ID3D12CommandQueue* queue = nullptr; ID3D12CommandAllocator* alloc = nullptr; ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr; uint64_t value = 0; HANDLE event = nullptr;
    bool Init() {
        IDXGIFactory6* f = nullptr; if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) return false;
        IDXGIAdapter1* a = nullptr; f->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)); f->Release();
        const HRESULT hr = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)); SafeRelease(a);
        if (FAILED(hr)) return false;
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&q, IID_PPV_ARGS(&queue))) || FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
            FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        list->Close();
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return true;
    }
    void Begin() { alloc->Reset(); list->Reset(alloc, nullptr); }
    void Submit() {   // runs the list and waits for it
        list->Close(); ID3D12CommandList* l[] = { list }; queue->ExecuteCommandLists(1, l);
        queue->Signal(fence, ++value); fence->SetEventOnCompletion(value, event); WaitForSingleObject(event, INFINITE);
    }
    void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        if (from == to) return;
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = from; b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
    }
    ID3D12Resource* Texture(uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool uav, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.SampleDesc.Count = 1; d.Format = fmt; d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        ID3D12Resource* r = nullptr; dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)); return r;
    }
    ID3D12Resource* Buffer(uint64_t size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* r = nullptr; dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)); return r;
    }
};

// ---- XeSS frame generation: it presents through the application's swap chain from a thread of its own and hands no frame back, so every
// frame it presents is copied out at that swap chain's Present (a hook on this process's own swap chain, as the addon hooks Lossless
// Scaling's), into a ring of readback buffers.
struct PresentCapture {
    IDXGISwapChain3* chain = nullptr; ID3D12Device* dev = nullptr; ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr; ID3D12GraphicsCommandList* list = nullptr; ID3D12Fence* fence = nullptr; uint64_t value = 0;
    static const int kRing = 8;
    ID3D12Resource* ring[kRing] = {}; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    volatile LONG count = 0;   // presents captured; the n-th is in ring[n % kRing], done when fence >= n + 1
    using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    PresentFn present = nullptr; Present1Fn present1 = nullptr;
    CRITICAL_SECTION lock;
};
PresentCapture g_cap;

void CaptureBackBuffer(IDXGISwapChain* sc) {
    // whichever DXGI swap chain presents (Intel's runtime may present through one of its own, made from the one it was given)
    IDXGISwapChain3* chain3 = nullptr;
    if (!g_cap.fence || FAILED(sc->QueryInterface(IID_PPV_ARGS(&chain3)))) return;
    // on the queue that swap chain presents with (a D3D12 swap chain gives it as its "device"), so the copy comes after what was drawn into it
    ID3D12CommandQueue* queue = nullptr;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&queue))) || !queue) { queue = g_cap.queue; queue->AddRef(); }
    EnterCriticalSection(&g_cap.lock);
    const LONG n = g_cap.count;
    if (g_cap.fence->GetCompletedValue() < g_cap.value) { g_cap.fence->SetEventOnCompletion(g_cap.value, nullptr); }   // the list before is done
    ID3D12Resource* back = nullptr;
    if (SUCCEEDED(chain3->GetBuffer(chain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back)))) {
        g_cap.alloc->Reset(); g_cap.list->Reset(g_cap.alloc, nullptr);
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = back;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_cap.list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION to{ g_cap.ring[n % PresentCapture::kRing], D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; to.PlacedFootprint = g_cap.fp;
        D3D12_TEXTURE_COPY_LOCATION from{ back, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; from.SubresourceIndex = 0;
        g_cap.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter); g_cap.list->ResourceBarrier(1, &b);
        g_cap.list->Close(); ID3D12CommandList* l[] = { g_cap.list }; queue->ExecuteCommandLists(1, l);
        queue->Signal(g_cap.fence, ++g_cap.value);
        back->Release();
        InterlockedIncrement(&g_cap.count);
    }
    LeaveCriticalSection(&g_cap.lock);
    queue->Release();
    chain3->Release();
}
HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* sc, UINT sync, UINT flags) { CaptureBackBuffer(sc); return g_cap.present(sc, sync, flags); }
HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) { CaptureBackBuffer(sc); return g_cap.present1(sc, sync, flags, p); }
void PatchSlot(void** vtable, int slot, void* hook, void** original) {
    DWORD old = 0; VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *original = vtable[slot]; vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
}

// Motion vectors that follow object edges: each pixel's vector re-taken from its neighbourhood (5x5 samples, 4 px apart), weighted by how
// alike each neighbour is to it in depth (when given) and in colour: a cross-bilateral filter of the motion field, guided by the picture. A
// pixel of a character then takes the character's motion and a pixel of the sky the sky's, where the estimate's 4x4 blocks straddle the edge.
// mv: RG half floats (W x H); rgba: the frame; depth: W x H (1 = near) or empty. sigmaDepth <= 0 leaves depth out, sigmaColour <= 0 colour.
void RefineMotion(std::vector<uint16_t>& mv, const std::vector<uint8_t>& rgba, const std::vector<float>& depth, uint32_t W, uint32_t H,
                  float sigmaDepth, float sigmaColour) {
    using DirectX::PackedVector::XMConvertHalfToFloat; using DirectX::PackedVector::XMConvertFloatToHalf;
    std::vector<float> vx(static_cast<size_t>(W) * H), vy(vx.size());
    for (size_t i = 0; i < vx.size(); ++i) { vx[i] = XMConvertHalfToFloat(mv[i * 2]); vy[i] = XMConvertHalfToFloat(mv[i * 2 + 1]); }
    const bool useDepth = sigmaDepth > 0 && depth.size() == vx.size(), useColour = sigmaColour > 0;
    const float kd = useDepth ? 1.0f / (2 * sigmaDepth * sigmaDepth) : 0, kc = useColour ? 1.0f / (2 * sigmaColour * sigmaColour) : 0;
    const int step = 4;
    #pragma omp parallel for schedule(dynamic, 16)
    for (int y = 0; y < static_cast<int>(H); ++y) for (int x = 0; x < static_cast<int>(W); ++x) {
        const size_t p = static_cast<size_t>(y) * W + x;
        const uint8_t* cp = rgba.data() + p * 4;
        float sw = 0, sx = 0, sy = 0;
        for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) {
            const int qx = std::clamp(x + dx * step, 0, static_cast<int>(W) - 1), qy = std::clamp(y + dy * step, 0, static_cast<int>(H) - 1);
            const size_t q = static_cast<size_t>(qy) * W + qx;
            float e = 0;
            if (useDepth) { const float d = depth[p] - depth[q]; e += d * d * kd; }
            if (useColour) { const uint8_t* cq = rgba.data() + q * 4; const float r = float(cp[0]) - cq[0], g = float(cp[1]) - cq[1], b = float(cp[2]) - cq[2]; e += (r * r + g * g + b * b) * kc; }
            const float w = std::exp(-e);
            sw += w; sx += w * vx[q]; sy += w * vy[q];
        }
        mv[p * 2] = XMConvertFloatToHalf(sx / sw); mv[p * 2 + 1] = XMConvertFloatToHalf(sy / sw);
    }
}

// The frame before moved by the vectors (each pixel of this frame taken from where its vector says it was, bilinear): as like this frame as
// the vectors are right. len: the average vector length (pixels).
void WarpByMotion(const std::vector<uint8_t>& before, const std::vector<uint16_t>& mv, uint32_t W, uint32_t H, std::vector<uint8_t>& out, double* len) {
    using DirectX::PackedVector::XMConvertHalfToFloat;
    out.resize(before.size()); double sum = 0;
    #pragma omp parallel for reduction(+ : sum)
    for (int y = 0; y < static_cast<int>(H); ++y) for (uint32_t x = 0; x < W; ++x) {
        const size_t p = static_cast<size_t>(y) * W + x;
        const float vx = XMConvertHalfToFloat(mv[p * 2]), vy = XMConvertHalfToFloat(mv[p * 2 + 1]);
        sum += std::sqrt(vx * vx + vy * vy);
        const float sx = std::clamp(x + vx, 0.0f, W - 1.0f), sy = std::clamp(y + vy, 0.0f, H - 1.0f);
        const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy), x1 = std::min(x0 + 1, static_cast<int>(W) - 1), y1 = std::min(y0 + 1, static_cast<int>(H) - 1);
        const float fx = sx - x0, fy = sy - y0;
        for (int c = 0; c < 4; ++c) {
            auto at = [&](int xx, int yy) { return static_cast<float>(before[(static_cast<size_t>(yy) * W + xx) * 4 + c]); };
            out[p * 4 + c] = static_cast<uint8_t>((at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy + 0.5f);
        }
    }
    if (len) *len = sum / (static_cast<double>(W) * H);
}

// A depth from the vectors, for a game that gives none (Lossless Scaling sees no depth): the picture's own motion (the median vector, the
// camera turning or moving) is taken as the far scene, and a pixel is the nearer the more its vector differs from it, fully near at `px`
// pixels (a third-person character the camera follows, which stays put while the scene sweeps past). depth: W x H, 1 = near, 0.1 = far.
void DepthFromMotion(const std::vector<uint16_t>& mv, uint32_t W, uint32_t H, std::vector<float>& depth, float px) {
    using DirectX::PackedVector::XMConvertHalfToFloat;
    std::vector<float> xs, ys;
    for (uint32_t y = 0; y < H; y += 8) for (uint32_t x = 0; x < W; x += 8) {
        const size_t p = static_cast<size_t>(y) * W + x; xs.push_back(XMConvertHalfToFloat(mv[p * 2])); ys.push_back(XMConvertHalfToFloat(mv[p * 2 + 1]));
    }
    std::nth_element(xs.begin(), xs.begin() + xs.size() / 2, xs.end()); std::nth_element(ys.begin(), ys.begin() + ys.size() / 2, ys.end());
    const float gx = xs[xs.size() / 2], gy = ys[ys.size() / 2];
    depth.resize(static_cast<size_t>(W) * H);
    #pragma omp parallel for
    for (int y = 0; y < static_cast<int>(H); ++y) for (uint32_t x = 0; x < W; ++x) {
        const size_t p = static_cast<size_t>(y) * W + x;
        const float dx = XMConvertHalfToFloat(mv[p * 2]) - gx, dy = XMConvertHalfToFloat(mv[p * 2 + 1]) - gy;
        depth[p] = 0.1f + 0.9f * std::clamp(std::sqrt(dx * dx + dy * dy) / px, 0.0f, 1.0f);
    }
}

// The frame between, matched at its own time (a yardstick: how far better placement of the vectors goes). Each pixel q of it tries its own
// vector, those of pixels `reach` away around it, "not moving" and the picture's median: the one under which the two kept frames, sampled
// at q -/+ v/2 over a 3x3, agree best. Where even the best disagrees (something covered or uncovered), the side that agrees with its own
// surroundings better (this frame at q - v/2, or the frame before at q + v/2) is taken alone instead of the mix, to leave no double image.
void MidMatch(const std::vector<uint8_t>& before, const std::vector<uint8_t>& now, const std::vector<uint16_t>& mv, uint32_t W, uint32_t H,
              std::vector<uint8_t>& out, int reach) {
    using DirectX::PackedVector::XMConvertHalfToFloat;
    out.resize(now.size());
    std::vector<float> xs, ys;
    for (uint32_t y = 0; y < H; y += 8) for (uint32_t x = 0; x < W; x += 8) {
        const size_t p = static_cast<size_t>(y) * W + x; xs.push_back(XMConvertHalfToFloat(mv[p * 2])); ys.push_back(XMConvertHalfToFloat(mv[p * 2 + 1]));
    }
    std::nth_element(xs.begin(), xs.begin() + xs.size() / 2, xs.end()); std::nth_element(ys.begin(), ys.begin() + ys.size() / 2, ys.end());
    const float gx = xs[xs.size() / 2], gy = ys[ys.size() / 2];
    auto luma = [&](const std::vector<uint8_t>& img, float sx, float sy) {   // nearest sample (the match), as brightness
        const int x = std::clamp(static_cast<int>(sx + 0.5f), 0, static_cast<int>(W) - 1), y = std::clamp(static_cast<int>(sy + 0.5f), 0, static_cast<int>(H) - 1);
        const uint8_t* p = img.data() + (static_cast<size_t>(y) * W + x) * 4; return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
    };
    auto sample = [&](const std::vector<uint8_t>& img, float sx, float sy, int c) {
        sx = std::clamp(sx, 0.0f, W - 1.0f); sy = std::clamp(sy, 0.0f, H - 1.0f);
        const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy), x1 = std::min(x0 + 1, static_cast<int>(W) - 1), y1 = std::min(y0 + 1, static_cast<int>(H) - 1);
        const float fx = sx - x0, fy = sy - y0;
        auto at = [&](int xx, int yy) { return static_cast<float>(img[(static_cast<size_t>(yy) * W + xx) * 4 + c]); };
        return (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
    };
    auto vec = [&](int x, int y) {
        x = std::clamp(x, 0, static_cast<int>(W) - 1); y = std::clamp(y, 0, static_cast<int>(H) - 1);
        const size_t p = static_cast<size_t>(y) * W + x; return std::pair<float, float>(XMConvertHalfToFloat(mv[p * 2]), XMConvertHalfToFloat(mv[p * 2 + 1]));
    };
    #pragma omp parallel for schedule(dynamic, 8)
    for (int y = 0; y < static_cast<int>(H); ++y) for (int x = 0; x < static_cast<int>(W); ++x) {
        std::pair<float, float> cand[11]; int n = 0;
        cand[n++] = vec(x, y); cand[n++] = { 0.0f, 0.0f }; cand[n++] = { gx, gy };
        for (int k = 0; k < 8; ++k) { static const int ox[8] = { -1, 0, 1, -1, 1, -1, 0, 1 }, oy[8] = { -1, -1, -1, 0, 0, 1, 1, 1 }; cand[n++] = vec(x + ox[k] * reach, y + oy[k] * reach); }
        float bestCost = 1e30f; std::pair<float, float> best = cand[0];
        for (int k = 0; k < n; ++k) {
            const float hx = 0.5f * cand[k].first, hy = 0.5f * cand[k].second; float cost = 0;
            for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) cost += std::abs(luma(now, x + i * 2 - hx, y + j * 2 - hy) - luma(before, x + i * 2 + hx, y + j * 2 + hy));
            if (k > 0) cost += 2.0f;   // the own vector wins a tie
            if (cost < bestCost) { bestCost = cost; best = cand[k]; }
        }
        const float hx = 0.5f * best.first, hy = 0.5f * best.second;
        const size_t p = static_cast<size_t>(y) * W + x;
        const float agree = bestCost / 9.0f;   // levels of brightness apart
        // one-sided where they disagree: the side whose vector the pixel's neighbourhood shares (its own sample matches the frame's own vector there)
        float wNow = 0.5f;
        if (agree > 12.0f) {
            const auto vn = vec(static_cast<int>(x - hx), static_cast<int>(y - hy));        // the vector this frame has where q comes from
            const float dn = std::abs(vn.first - best.first) + std::abs(vn.second - best.second);
            // this frame's pixel there moves with the chosen vector: this frame shows it (the frame before may have it covered)
            wNow = dn < 2.0f ? 0.85f : 0.15f;
        }
        for (int c = 0; c < 4; ++c) out[p * 4 + c] = static_cast<uint8_t>(wNow * sample(now, x - hx, y - hy, c) + (1 - wNow) * sample(before, x + hx, y + hy, c) + 0.5f);
    }
}

// The camera's own motion, fitted to the vectors: a turning camera moves the picture by u = a0 + a1 x + a2 y + a6 x^2 + a7 xy,
// v = a3 + a4 x + a5 y + a6 xy + a7 y^2 (x, y from -1 to 1 across the picture; the eight-parameter model of a camera rotating, and of
// a far scene), fitted by least squares over one sample per 8x8 block, three more times without the samples furthest off (the character,
// wrong vectors). What departs from it moves on its own (or is near: parallax).
struct CameraMotion {
    double a[8] = {};
    bool ok = false;
    float At(float px, float py, uint32_t W, uint32_t H, float* vy) const {
        const double x = 2.0 * px / W - 1.0, y = 2.0 * py / H - 1.0;
        *vy = static_cast<float>(a[3] + a[4] * x + a[5] * y + a[6] * x * y + a[7] * y * y);
        return static_cast<float>(a[0] + a[1] * x + a[2] * y + a[6] * x * x + a[7] * x * y);
    }
};
CameraMotion FitCamera(const std::vector<uint16_t>& mv, uint32_t W, uint32_t H) {
    using DirectX::PackedVector::XMConvertHalfToFloat;
    struct S { double x, y, u, v; bool in; };
    std::vector<S> s;
    for (uint32_t y = 4; y < H; y += 8) for (uint32_t x = 4; x < W; x += 8) {
        const size_t p = static_cast<size_t>(y) * W + x;
        s.push_back({ 2.0 * x / W - 1.0, 2.0 * y / H - 1.0, XMConvertHalfToFloat(mv[p * 2]), XMConvertHalfToFloat(mv[p * 2 + 1]), true });
    }
    CameraMotion m;
    for (int pass = 0; pass < 4; ++pass) {
        double A[8][9] = {};   // normal equations, augmented
        for (const S& q : s) {
            if (!q.in) continue;
            const double ru[8] = { 1, q.x, q.y, 0, 0, 0, q.x * q.x, q.x * q.y }, rv[8] = { 0, 0, 0, 1, q.x, q.y, q.x * q.y, q.y * q.y };
            for (int i = 0; i < 8; ++i) { for (int j = 0; j < 8; ++j) A[i][j] += ru[i] * ru[j] + rv[i] * rv[j]; A[i][8] += ru[i] * q.u + rv[i] * q.v; }
        }
        for (int i = 0; i < 8; ++i) A[i][i] += 1e-6;
        for (int c = 0; c < 8; ++c) {   // Gauss-Jordan with partial pivoting
            int piv = c; for (int r = c + 1; r < 8; ++r) if (std::abs(A[r][c]) > std::abs(A[piv][c])) piv = r;
            if (std::abs(A[piv][c]) < 1e-12) return m;
            for (int k = 0; k < 9; ++k) std::swap(A[c][k], A[piv][k]);
            for (int r = 0; r < 8; ++r) if (r != c) { const double f = A[r][c] / A[c][c]; for (int k = c; k < 9; ++k) A[r][k] -= f * A[c][k]; }
        }
        for (int i = 0; i < 8; ++i) m.a[i] = A[i][8] / A[i][i];
        m.ok = true;
        if (pass == 3) break;
        std::vector<double> res;   // keep the samples within twice the median distance from the fit (at least 2 px)
        for (const S& q : s) { float vy; const double ux = m.At(static_cast<float>((q.x + 1) * W / 2), static_cast<float>((q.y + 1) * H / 2), W, H, &vy); res.push_back(std::hypot(q.u - ux, q.v - vy)); }
        std::vector<double> sorted = res; std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        const double keep = std::max(2.0, 2.0 * sorted[sorted.size() / 2]);
        for (size_t k = 0; k < s.size(); ++k) s[k].in = res[k] <= keep;
    }
    return m;
}

// The 8x8 blocks that move on their own: most of their pixels depart from the camera's motion (FitCamera) by a quarter of its speed there
// (at least 4 px), and at least `neighbours` of the 8 blocks around do too (a character, not foliage whose vectors stray). Empty when the
// camera hardly moves (under 8 px at the picture's centre). bw, bh: the grid's size.
std::vector<uint8_t> OwnMotionBlocks(const std::vector<uint16_t>& mv, uint32_t W, uint32_t H, int neighbours, int& bw, int& bh, CameraMotion* camOut = nullptr) {
    using DirectX::PackedVector::XMConvertHalfToFloat;
    bw = static_cast<int>((W + 7) / 8); bh = static_cast<int>((H + 7) / 8);
    std::vector<uint8_t> fg(static_cast<size_t>(bw) * bh, 0), kept(fg.size(), 0);
    const CameraMotion cam = FitCamera(mv, W, H);
    if (camOut) *camOut = cam;
    float cy; const float cx = cam.ok ? cam.At(W * 0.5f, H * 0.5f, W, H, &cy) : (cy = 0.0f);
    if (!cam.ok || std::hypot(cx, cy) < 8.0f) return kept;
    for (int by = 0; by < bh; ++by) for (int bx = 0; bx < bw; ++bx) {
        int n = 0, in = 0;
        for (uint32_t y = by * 8; y < std::min<uint32_t>(H, by * 8 + 8); y += 2) for (uint32_t x = bx * 8; x < std::min<uint32_t>(W, bx * 8 + 8); x += 2) {
            const size_t p = static_cast<size_t>(y) * W + x;
            float ey; const float ex = cam.At(static_cast<float>(x), static_cast<float>(y), W, H, &ey);
            const float t = std::max(4.0f, 0.25f * std::hypot(ex, ey));
            const float dx = XMConvertHalfToFloat(mv[p * 2]) - ex, dy = XMConvertHalfToFloat(mv[p * 2 + 1]) - ey;
            ++n; in += dx * dx + dy * dy > t * t ? 1 : 0;
        }
        fg[static_cast<size_t>(by) * bw + bx] = in * 2 > n ? 1 : 0;
    }
    for (int by = 0; by < bh; ++by) for (int bx = 0; bx < bw; ++bx) {
        if (!fg[static_cast<size_t>(by) * bw + bx]) continue;
        int agree = 0;
        for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
            const int x = bx + i, y = by + j;
            if ((i || j) && x >= 0 && y >= 0 && x < bw && y < bh && fg[static_cast<size_t>(y) * bw + x]) ++agree;
        }
        kept[static_cast<size_t>(by) * bw + bx] = agree >= neighbours ? 1 : 0;
    }
    return kept;
}

// A depth read from the motion of a camera orbiting what it follows (a third-person camera turning): a point at the pivot (the character)
// stays put, the far scene moves the most in the turn's direction, nearer things less, things nearer than the pivot the other way. So each
// 8x8 block's motion along the turn's direction (its median, over the picture's 90th percentile, 0..1) says how far it is: 0 near (0.5 m)
// to 1 far (50 m), spread geometrically, smoothed over the 3x3 blocks around. A first-person turn (no orbit) moves everything alike and
// gives a flat depth. Flat (5 m) when the picture hardly moves (under 8 px). depth: W x H, device depth, inverted, for near 0.1 / far 1000.
void DepthOrbit(const std::vector<uint16_t>& mv, uint32_t W, uint32_t H, std::vector<float>& depth) {
    using DirectX::PackedVector::XMConvertHalfToFloat;
    auto device = [](float metres) { return 0.1f / metres; };
    depth.assign(static_cast<size_t>(W) * H, device(5.0f));
    std::vector<float> xs, ys;
    for (uint32_t y = 4; y < H; y += 8) for (uint32_t x = 4; x < W; x += 8) {
        const size_t p = static_cast<size_t>(y) * W + x; xs.push_back(XMConvertHalfToFloat(mv[p * 2])); ys.push_back(XMConvertHalfToFloat(mv[p * 2 + 1]));
    }
    std::vector<float> sx = xs, sy = ys;
    std::nth_element(sx.begin(), sx.begin() + sx.size() / 2, sx.end()); std::nth_element(sy.begin(), sy.begin() + sy.size() / 2, sy.end());
    const float gx = sx[sx.size() / 2], gy = sy[sy.size() / 2], g = std::hypot(gx, gy);
    if (g < 8.0f) return;
    const float dx = gx / g, dy = gy / g;
    const int bw = static_cast<int>((W + 7) / 8), bh = static_cast<int>((H + 7) / 8);
    std::vector<float> s(static_cast<size_t>(bw) * bh);
    for (int by = 0; by < bh; ++by) for (int bx = 0; bx < bw; ++bx) {   // each block's median motion along the turn
        float v[16]; int n = 0;
        for (uint32_t y = by * 8 + 1; y < std::min<uint32_t>(H, by * 8 + 8); y += 2) for (uint32_t x = bx * 8 + 1; x < std::min<uint32_t>(W, bx * 8 + 8); x += 2) {
            const size_t p = static_cast<size_t>(y) * W + x;
            v[n++] = XMConvertHalfToFloat(mv[p * 2]) * dx + XMConvertHalfToFloat(mv[p * 2 + 1]) * dy;
        }
        if (!n) { s[static_cast<size_t>(by) * bw + bx] = g; continue; }
        std::nth_element(v, v + n / 2, v + n); s[static_cast<size_t>(by) * bw + bx] = v[n / 2];
    }
    std::vector<float> sorted = s; std::nth_element(sorted.begin(), sorted.begin() + sorted.size() * 9 / 10, sorted.end());
    const float farthest = std::max(sorted[sorted.size() * 9 / 10], 1.0f);
    std::vector<float> t(s.size());
    for (int by = 0; by < bh; ++by) for (int bx = 0; bx < bw; ++bx) {   // 0..1, smoothed over 3x3 blocks
        float sum = 0; int n = 0;
        for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
            const int x = bx + i, y = by + j;
            if (x >= 0 && y >= 0 && x < bw && y < bh) { sum += std::clamp(s[static_cast<size_t>(y) * bw + x] / farthest, 0.0f, 1.0f); ++n; }
        }
        t[static_cast<size_t>(by) * bw + bx] = sum / n;
    }
    #pragma omp parallel for
    for (int y = 0; y < static_cast<int>(H); ++y) for (uint32_t x = 0; x < W; ++x) {   // bilinear between block centres
        const float fx = std::clamp((x + 0.5f) / 8.0f - 0.5f, 0.0f, bw - 1.0f), fy = std::clamp((y + 0.5f) / 8.0f - 0.5f, 0.0f, bh - 1.0f);
        const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy), x1 = std::min(x0 + 1, bw - 1), y1 = std::min(y0 + 1, bh - 1);
        const float ax = fx - x0, ay = fy - y0;
        const float k = (t[y0 * bw + x0] * (1 - ax) + t[y0 * bw + x1] * ax) * (1 - ay) + (t[y1 * bw + x0] * (1 - ax) + t[y1 * bw + x1] * ax) * ay;
        depth[static_cast<size_t>(y) * W + x] = device(0.5f * std::pow(100.0f, k));
    }
}

// A depth of two layers, for FSR's priority where two motions land on the same pixel of the frame between (FSR takes the nearer in view
// space; with a flat depth the better colour match wins, and in a turn that is often the background sweeping over the character): near
// where the blocks move on their own (OwnMotionBlocks), far elsewhere. depth: W x H, 1 = near, 0.1 = far (inverted). Returns the near fraction.
double DepthLayers(const std::vector<uint16_t>& mv, uint32_t W, uint32_t H, std::vector<float>& depth, int neighbours) {
    depth.assign(static_cast<size_t>(W) * H, 0.1f);
    int bw = 0, bh = 0; CameraMotion cam;
    const std::vector<uint8_t> own = OwnMotionBlocks(mv, W, H, neighbours, bw, bh, &cam);
    size_t nearCount = 0;
    for (int by = 0; by < bh; ++by) for (int bx = 0; bx < bw; ++bx) {
        if (!own[static_cast<size_t>(by) * bw + bx]) continue;
        for (uint32_t y = by * 8; y < std::min<uint32_t>(H, by * 8 + 8); ++y) for (uint32_t x = bx * 8; x < std::min<uint32_t>(W, bx * 8 + 8); ++x) { depth[static_cast<size_t>(y) * W + x] = 1.0f; ++nearCount; }
    }
    float cy; const float cx = cam.ok ? cam.At(W * 0.5f, H * 0.5f, W, H, &cy) : (cy = 0.0f);
    printf("  layers: the camera moves the centre (%.0f, %.0f) px; %.1f%% near\n", cx, cy, 100.0 * nearCount / (static_cast<double>(W) * H));
    return static_cast<double>(nearCount) / (static_cast<double>(W) * H);
}

// The guard against pasted background: where the two real frames agree around a pixel (within `r` pixels, a character that shifts a
// little) and the frame made between is far from both, it was pasted over (the background sweeping past in a turn winning over a character
// FSR saw as no nearer): the two real frames' mix goes there instead. Where the real frames differ (the moving scene), the frame made stays.
// agree / apart: brightness levels (0..255). Returns the share of the picture replaced.
// Only where the motion estimate calls the picture slow (under a fifth of the picture's median motion, at least 3 px; mv: this frame's
// vectors, RG half floats; empty: everywhere): a leaf moving through the canopy is at the frame between's place in neither real frame, and
// the frame made is right to show it there.
double Guard(std::vector<uint8_t>& made, const std::vector<uint8_t>& before, const std::vector<uint8_t>& now, uint32_t W, uint32_t H, int r, float agree, float apart,
             const std::vector<uint16_t>& mv = {}) {
    using DirectX::PackedVector::XMConvertHalfToFloat;
    float slow = 1e9f;
    if (mv.size() >= static_cast<size_t>(W) * H * 2) {
        std::vector<float> len;
        for (size_t p = 0; p < static_cast<size_t>(W) * H; p += 61) len.push_back(std::hypot(XMConvertHalfToFloat(mv[p * 2]), XMConvertHalfToFloat(mv[p * 2 + 1])));
        std::nth_element(len.begin(), len.begin() + len.size() / 2, len.end());
        slow = std::max(3.0f, 0.2f * len[len.size() / 2]);
    }
    auto slowAt = [&](int x, int y) {
        x = std::clamp(x, 0, static_cast<int>(W) - 1); y = std::clamp(y, 0, static_cast<int>(H) - 1);
        const size_t p = static_cast<size_t>(y) * W + x;
        return std::hypot(XMConvertHalfToFloat(mv[p * 2]), XMConvertHalfToFloat(mv[p * 2 + 1])) <= slow;
    };
    auto isSlow = [&](int x, int y) {   // slow here, and around (5 of 8 on a ring 3 reaches out: a character, not a gap of sky in a moving canopy)
        if (slow >= 1e9f) return true;
        if (!slowAt(x, y)) return false;
        int around = 0;
        for (int k = 0; k < 8; ++k) around += slowAt(x + static_cast<int>(std::lround(std::cos(k * 0.785398) * 3.0 * r)), y + static_cast<int>(std::lround(std::sin(k * 0.785398) * 3.0 * r))) ? 1 : 0;
        return around >= 5;
    };
    // distances in colour (the largest channel difference): a pasted leaf can be about as bright as the hair it covers, not as blue
    auto at = [&](const std::vector<uint8_t>& img, int x, int y) {
        x = std::clamp(x, 0, static_cast<int>(W) - 1); y = std::clamp(y, 0, static_cast<int>(H) - 1);
        return img.data() + (static_cast<size_t>(y) * W + x) * 4;
    };
    auto dist = [](const uint8_t* a, const uint8_t* b) { return static_cast<float>(std::max({ std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2]) })); };
    std::vector<uint8_t> out = made; long long replaced = 0;
    #pragma omp parallel for reduction(+ : replaced) schedule(dynamic, 8)
    for (int y = 0; y < static_cast<int>(H); ++y) for (int x = 0; x < static_cast<int>(W); ++x) {
        if (!isSlow(x, y)) continue;
        const uint8_t *pb = at(before, x, y), *pn = at(now, x, y), *pm = at(made, x, y);
        float dBN = dist(pb, pn), dMB = dist(pm, pb), dMN = dist(pm, pn);
        if (dBN > agree) {   // not alike here: alike a little way off? (the character shifts a few pixels)
            for (int j = -r; j <= r && dBN > agree; ++j) for (int i = -r; i <= r; ++i) dBN = std::min(dBN, dist(pb, at(now, x + i, y + j)));
        }
        if (dBN > agree) continue;
        if (std::min(dMB, dMN) <= dBN + apart) continue;
        for (int j = -r; j <= r; ++j) for (int i = -r; i <= r; ++i) { dMB = std::min(dMB, dist(pm, at(before, x + i, y + j))); dMN = std::min(dMN, dist(pm, at(now, x + i, y + j))); }
        const float t = std::clamp((std::min(dMB, dMN) - dBN - apart) / apart, 0.0f, 1.0f);   // 0 keep the frame made .. 1 the real frames' mix
        if (t <= 0.0f) continue;
        const size_t p = (static_cast<size_t>(y) * W + x) * 4;
        for (int c = 0; c < 3; ++c) out[p + c] = static_cast<uint8_t>(made[p + c] * (1 - t) + 0.5f * (before[p + c] + now[p + c]) * t + 0.5f);
        ++replaced;
    }
    made.swap(out);
    return static_cast<double>(replaced) / (static_cast<double>(W) * H);
}

// Where turn ghosting shows (the character a camera follows, pasted over by the background sweeping past): the blocks that move on their own
// (OwnMotionBlocks, 3 neighbours), widened by half the camera's speed at the centre (at least 8 px). Empty when the camera hardly moves.
// Returns the fraction of the picture it covers.
double BandMask(const std::vector<uint16_t>& mv, uint32_t W, uint32_t H, std::vector<uint8_t>& mask) {
    mask.assign(static_cast<size_t>(W) * H, 0);
    int bw = 0, bh = 0; CameraMotion cam;
    const std::vector<uint8_t> fg = OwnMotionBlocks(mv, W, H, 3, bw, bh, &cam);
    float cy; const float cx = cam.ok ? cam.At(W * 0.5f, H * 0.5f, W, H, &cy) : (cy = 0.0f);
    const int r = static_cast<int>(std::ceil(std::max(8.0f, 0.5f * std::hypot(cx, cy)) / 8.0f));
    size_t covered = 0;
    for (int by = 0; by < bh; ++by) for (int bx = 0; bx < bw; ++bx) {
        bool beside = false;
        for (int j = -r; j <= r && !beside; ++j) for (int i = -r; i <= r && !beside; ++i) {
            const int x = bx + i, y = by + j;
            beside = x >= 0 && y >= 0 && x < bw && y < bh && fg[static_cast<size_t>(y) * bw + x];
        }
        if (!beside) continue;
        for (uint32_t y = by * 8; y < std::min<uint32_t>(H, by * 8 + 8); ++y) for (uint32_t x = bx * 8; x < std::min<uint32_t>(W, bx * 8 + 8); ++x) { mask[static_cast<size_t>(y) * W + x] = 1; ++covered; }
    }
    return static_cast<double>(covered) / (static_cast<double>(W) * H);
}

double PsnrMasked(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const std::vector<uint8_t>& mask) {
    double se = 0; size_t n = 0;
    for (size_t p = 0; p < mask.size(); ++p) if (mask[p]) for (int c = 0; c < 3; ++c) { const double d = double(a[p * 4 + c]) - double(b[p * 4 + c]); se += d * d; ++n; }
    const double mse = n ? se / n : 0;
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// The frame between, from the vectors alone (the simplest interpolation, a yardstick for the generators): a pixel of it at p, at time t
// (0 the frame before, 1 this one), is taken from this frame at p - (1 - t) v and from the frame before at p + t v (v: the vector at p,
// standing for the one at the frame between), mixed by t.
void HalfWay(const std::vector<uint8_t>& before, const std::vector<uint8_t>& now, const std::vector<uint16_t>& mv, uint32_t W, uint32_t H, std::vector<uint8_t>& out, float t) {
    using DirectX::PackedVector::XMConvertHalfToFloat;
    out.resize(now.size());
    auto sample = [&](const std::vector<uint8_t>& img, float sx, float sy, int c) {
        sx = std::clamp(sx, 0.0f, W - 1.0f); sy = std::clamp(sy, 0.0f, H - 1.0f);
        const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy), x1 = std::min(x0 + 1, static_cast<int>(W) - 1), y1 = std::min(y0 + 1, static_cast<int>(H) - 1);
        const float fx = sx - x0, fy = sy - y0;
        auto at = [&](int xx, int yy) { return static_cast<float>(img[(static_cast<size_t>(yy) * W + xx) * 4 + c]); };
        return (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
    };
    #pragma omp parallel for
    for (int y = 0; y < static_cast<int>(H); ++y) for (uint32_t x = 0; x < W; ++x) {
        const size_t p = static_cast<size_t>(y) * W + x;
        const float vx = XMConvertHalfToFloat(mv[p * 2]), vy = XMConvertHalfToFloat(mv[p * 2 + 1]);
        for (int c = 0; c < 4; ++c)
            out[p * 4 + c] = static_cast<uint8_t>(t * sample(now, x - (1 - t) * vx, y - (1 - t) * vy, c) + (1 - t) * sample(before, x + t * vx, y + t * vy, c) + 0.5f);
    }
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { printf("usage: nr_fgeval <recording.lsrec> <output folder> [gen=fsr|xess] [first=N] [count=N] [worst=N] [fsr=<amd_fidelityfx_dx12.dll>] [xessfg=<libxess_fg.dll>] [estin=light|view] [depth=motion] [keepstill=N] [mvcheck=1] [live=1]\n"); return 2; }
    nr::lsrec::Reader rec; std::string error;
    if (!rec.Open(Wide(argv[1]), &error)) { printf("%s: %s\n", argv[1], error.c_str()); return 2; }
    std::wstring outDir = Wide(argv[2]);
    { wchar_t full[MAX_PATH]; if (GetFullPathNameW(outDir.c_str(), MAX_PATH, full, nullptr)) outDir = full; SHCreateDirectoryExW(nullptr, outDir.c_str(), nullptr); }   // with its parents
    const nr::lsrec::FileHeader& h = rec.Header();
    const uint32_t W = h.width, H = h.height;
    const int first = std::max(0, Arg(argc, argv, "first", 0));
    int count = std::min<int>(Arg(argc, argv, "count", 1 << 30), static_cast<int>(rec.Count()) - first);   // (fewer with realonly=1)
    const int worstShown = Arg(argc, argv, "worst", 3);
    const bool xess = ArgText(argc, argv, "gen") == "xess";
    const char* const G = xess ? "XeSS" : "FSR";
    // depthdir=<folder>: depth per frame (depth_NNNNN.bin, width x height floats, 1 = near: tools\depth_maps.py) in place of a flat depth
    const std::wstring depthDir = Wide(ArgText(argc, argv, "depthdir").c_str());
    // depthinv=0: the depth files hold 1 = far (the generators told depth is not inverted); for checking how depth is read
    const bool depthNearIsOne = Arg(argc, argv, "depthinv", 1) != 0;
    // motion=none: the generators get zero motion vectors (to see how much they rely on ours, against their own optical flow)
    const bool noMotion = ArgText(argc, argv, "motion") == "none";
    // keepstill=N: where the two real frames are alike (their 3x3 surroundings N levels apart or less, fading out by 2N), the frame between
    // is their mix instead of the generator's (KeepStill)
    const int keepStill = Arg(argc, argv, "keepstill", 0);
    // refine=depth|colour|both: the motion vectors refined along depth and / or colour edges (RefineMotion; depth needs depthdir=)
    const std::string refine = ArgText(argc, argv, "refine");
    const float sigmaDepth = refine == "depth" || refine == "both" ? Arg(argc, argv, "sd", 50) / 1000.0f : 0.0f;       // sd= in thousandths
    const float sigmaColour = refine == "colour" || refine == "both" ? static_cast<float>(Arg(argc, argv, "sc", 20)) : 0.0f;   // sc= in levels
    // mvcheck=1: how right the motion estimate is, apart from the generator: the kept frame before, moved by the vectors, scored against this
    // kept frame ("moved"), beside the kept frame before as it is ("still"), and the average vector length
    const bool mvCheck = Arg(argc, argv, "mvcheck", 0) != 0;
    // mvscale=N: the vectors scaled by N/100 for FSR (100: as they are); for checking how FSR reads them
    const float mvScale = Arg(argc, argv, "mvscale", 100) / 100.0f;
    // depth=motion: a depth made from the vectors (DepthFromMotion: what moves unlike the picture as a whole is near) in place of a flat one;
    // depthpx=N: the difference from the picture's motion (pixels) that counts as fully near
    const bool depthFromMotion = ArgText(argc, argv, "depth") == "motion", depthNoise = ArgText(argc, argv, "depth") == "noise", depthLayer = ArgText(argc, argv, "depth") == "layer", depthOrbit = ArgText(argc, argv, "depth") == "orbit";
    const int layerNeighbours = Arg(argc, argv, "layern", 5);   // depth=layer: how many of the 8 blocks around a near block must be near too
    // worstby=band: the worst frames pictured are those worst around the character (the band score), not over the whole picture
    const bool worstByBand = ArgText(argc, argv, "worstby") == "band";
    // ffxdebug=1: FidelityFX's checks and messages; debugview=1: FSR draws its own views of its inputs into the frame it makes
    const bool ffxDebug = Arg(argc, argv, "ffxdebug", 0) != 0, debugView = Arg(argc, argv, "debugview", 0) != 0;
    const bool mvDump = Arg(argc, argv, "mvdump", 0) != 0;
    // direct=1: FSR frame generation dispatched on our own command list (no FidelityFX swap chain, no presents, no pacing); else through its swap chain
    const bool direct = Arg(argc, argv, "direct", 0) != 0;
    // engine=1: the addon's own FgEngine (src/engine/fg_engine.cpp) makes the frames, through shared textures and fences, as live
    const bool useEngine = Arg(argc, argv, "engine", 0) != 0;
    // guard=N: the guard against pasted background (Guard): the frame made must be N levels further from both real frames than they are from
    // each other; guardagree=N: how alike the real frames must be (levels); the reach for a character's shift scales with the width
    const int guardApart = Arg(argc, argv, "guard", 0), guardAgree = Arg(argc, argv, "guardagree", 16);
    const int guardRadius = std::max(2, static_cast<int>(h.width / 640));   // mvdump=1: each kept frame's vectors as a picture
    // estin=light|view: the motion estimate fed the frames as scRGB (half floats), taken as they are (light) or in their SDR view (view)
    const std::string estIn = ArgText(argc, argv, "estin");
    // live=1: as live, a frame made between every two frames of the recording (none dropped, so nothing to score against): each kept as a
    // picture, to be looked at
    const int stride = Arg(argc, argv, "live", 0) ? 1 : 2;
    if (stride == 1 && mvCheck) { printf("mvcheck= needs frames dropped (not live=1)\n"); return 2; }
    const float depthPx = static_cast<float>(Arg(argc, argv, "depthpx", 8));
    if (count < 3) { printf("the recording has too few frames (%zu)\n", rec.Count()); return 2; }
    std::wstring exeDir; { wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH); exeDir = exe; exeDir = exeDir.substr(0, exeDir.find_last_of(L'\\')); }

    // the frames, in their SDR view
    std::vector<std::vector<uint8_t>> frames(count); std::vector<double> times(count);
    { std::vector<uint8_t> px;
      // realonly=1: a recording of what was shown (frame generation of our own) holds frames made between too: only the real ones are taken,
      // which is exactly what the engine was given live
      const bool realOnly = Arg(argc, argv, "realonly", 0) != 0;
      int kept = 0;
      for (int i = 0; first + i < static_cast<int>(rec.Count()) && kept < count; ++i) {
          if (realOnly && rec.FrameInfo(first + i).tag == nr::lsrec::kMadeBetween) continue;
          if (!rec.Read(first + i, px) || !ToRgba8(h, px, frames[kept])) { printf("frame %d could not be read or converted (format %u)\n", first + i, h.format); return 3; }
          times[kept] = h.qpcFrequency ? rec.FrameInfo(first + i).qpc * 1000.0 / static_cast<double>(h.qpcFrequency) : i * 16.7;   // ms
          ++kept;
      }
      if (kept < count) { frames.resize(kept); times.resize(kept); count = kept; }
      if (count < 3) { printf("too few frames\n"); return 2; } }
    printf("%ls: %ux%u, %d frames from %d; every other one is rebuilt by %s frame generation\n", Wide(argv[1]).c_str(), W, H, count, first, G);

    Gpu g; if (!g.Init()) { printf("no Direct3D 12 device\n"); return 4; }
    FlowEstimator est;
    if (!est.Init(g.dev, [](const char* m) { printf("  %s\n", m); }) || !est.Ensure(W, H)) { printf("the motion estimate could not start\n"); return 4; }
    if (const int cap = Arg(argc, argv, "straycap", -1); cap >= 0) est.SetStrayCap(cap == 0 ? 1e9f : static_cast<float>(cap));   // straycap=N (0: uncapped, as before)
    if (const int mw = Arg(argc, argv, "meanweight", -1); mw >= 0) est.SetMeanWeight(mw / 100.0f);   // meanweight=N: percent (100: the plain difference)
    if (const int gw = Arg(argc, argv, "gradweight", -1); gw >= 0) est.SetGradWeight(gw / 100.0f);   // gradweight=N: percent the edges count (0: the plain difference)

    // textures: the kept frame (read by the estimate and the generator), the motion vectors and the distrust mask, a flat depth
    ID3D12Resource* cur = g.Texture(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* motion = g.Texture(W, H, DXGI_FORMAT_R16G16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* distrust = g.Texture(W, H, DXGI_FORMAT_R8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* depth = g.Texture(W, H, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_DESC texDesc = cur->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    g.dev->GetCopyableFootprints(&texDesc, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    ID3D12Resource* upload = g.Buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* readback = g.Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!cur || !motion || !distrust || !depth || !upload || !readback) { printf("textures could not be made\n"); return 4; }
    // the depth: flat (Lossless Scaling has none), or each frame's from depthdir=; written on g.list (open), left readable
    D3D12_RESOURCE_DESC depthDesc = depth->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT dfp{}; UINT drows = 0; UINT64 drow = 0, dtotal = 0;
    g.dev->GetCopyableFootprints(&depthDesc, 0, 1, 0, &dfp, &drows, &drow, &dtotal);
    ID3D12Resource* depthUpload = g.Buffer(dtotal, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COPY_DEST;
    auto writeDepth = [&](const float* values) {   // W x H values; nullptr: flat
        uint8_t* m = nullptr; depthUpload->Map(0, nullptr, reinterpret_cast<void**>(&m));
        const std::vector<float> flat(W, 0.5f);
        for (UINT y = 0; y < H; ++y) memcpy(m + dfp.Offset + y * dfp.Footprint.RowPitch, values ? values + static_cast<size_t>(y) * W : flat.data(), W * 4);
        depthUpload->Unmap(0, nullptr);
        g.Barrier(depth, depthState, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to{ depth, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; to.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION from{ depthUpload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; from.PlacedFootprint = dfp;
        g.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        g.Barrier(depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    };
    g.Begin(); writeDepth(nullptr); g.Submit();
    std::vector<float> depthValues;
    auto loadDepth = [&](int recordingIndex) -> bool {
        wchar_t name[40]; swprintf(name, 40, L"\\depth_%05d.bin", recordingIndex);
        FILE* f = _wfopen((depthDir + name).c_str(), L"rb"); if (!f) return false;
        depthValues.resize(static_cast<size_t>(W) * H);
        const size_t got = fread(depthValues.data(), sizeof(float), depthValues.size(), f); fclose(f);
        return got == depthValues.size();
    };
    if (!depthDir.empty()) printf("  depth from %ls (1 = near)\n", depthDir.c_str());
    auto readPicture = [&](ID3D12Resource* buffer, std::vector<uint8_t>& out) {
        out.resize(static_cast<size_t>(W) * H * 4);
        uint8_t* m = nullptr; buffer->Map(0, nullptr, reinterpret_cast<void**>(&m));
        for (uint32_t y = 0; y < H; ++y) memcpy(out.data() + static_cast<size_t>(y) * W * 4, m + fp.Offset + y * fp.Footprint.RowPitch, W * 4);
        D3D12_RANGE none{ 0, 0 }; buffer->Unmap(0, &none);
    };
    // copies the kept frame (in cur, readable) into a back buffer, on g.list
    auto frameIntoBackBuffer = [&](ID3D12Resource* back) {
        g.Barrier(cur, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g.Barrier(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        g.list->CopyResource(back, cur);
        g.Barrier(back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        g.Barrier(cur, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    };

    // The generators run at present time, on a swap chain of their own (as in a game with them built in), on a window that is never shown.
    // step(i, frameMs, picture): with g.list open, the kept frame i in cur and its motion in motion (readable), it submits the list,
    // presents, and for i >= 2 returns the frame generated between the kept frame before and this one.
    WNDCLASSEXW wc{ sizeof wc }; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"NrFgEval"; RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"nr fgeval", WS_POPUP, 0, 0, 320, 180, nullptr, nullptr, wc.hInstance, nullptr);
    IDXGIFactory4* factory = nullptr; CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = W; sd.Height = H; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 3; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    std::function<bool(int, float, std::vector<uint8_t>&)> step;

    // -- AMD FSR 3.1: FidelityFX's swap chain (as OptiScaler sets it up); the frame is taken in the generation callback, as it is made
    ffxFunctions fx{}; ffxContext ctx = nullptr, chainCtx = nullptr; IDXGISwapChain4* chain = nullptr; ffxConfigureDescFrameGeneration cfg{};
    static ffxFunctions* s_fx = &fx; static ID3D12Resource* s_readback = readback; static D3D12_PLACED_SUBRESOURCE_FOOTPRINT s_fp = fp; static volatile LONG s_generated = 0;
    // -- Intel XeSS frame generation: the application's swap chain wrapped by Intel's; frames taken at that swap chain's Present
#if NR_HAVE_XEFG
    HMODULE xefgModule = nullptr; xefg_swapchain_handle_t xefg = nullptr; IDXGISwapChain4* proxy = nullptr;
    decltype(&xefgSwapChainD3D12TagFrameResource) xTag = nullptr; decltype(&xefgSwapChainTagFrameConstants) xConst = nullptr;
    decltype(&xefgSwapChainSetPresentId) xPresentId = nullptr; decltype(&xefgSwapChainDestroy) xDestroy = nullptr;
    decltype(&xefgSwapChainGetLastPresentStatus) xStatus = nullptr;
    // Intel's Xe Low Latency: XeSS frame generation starts only with it, and takes the frame's stages as markers
    HMODULE xellModule = nullptr; xell_context_handle_t xell = nullptr;
    decltype(&xellAddMarkerData) llMark = nullptr; decltype(&xellDestroyContext) llDestroy = nullptr;
#endif

    if (!xess) {
        std::wstring dll = Wide(ArgText(argc, argv, "fsr").c_str());
        if (dll.empty()) dll = exeDir + L"\\fsr\\amd_fidelityfx_dx12.dll";   // AMD's runtime, as the FSR Upscaler ships it
        HMODULE ffxModule = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!ffxModule) { printf("AMD's runtime could not be loaded from %ls\n", dll.c_str()); return 4; }
        ffxLoadFunctions(&fx, ffxModule);
        if (!fx.CreateContext || !fx.Configure || !fx.Dispatch) { printf("AMD's runtime lacks the FidelityFX API\n"); return 4; }
        if (fx.Query) {   // the frame generation versions the runtime holds (the first is the one used)
            uint64_t n = 8; uint64_t ids[8] = {}; const char* names[8] = {};
            ffxQueryDescGetVersions q{}; q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS; q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
            q.device = g.dev; q.outputCount = &n; q.versionIds = ids; q.versionNames = names;
            if (fx.Query(nullptr, &q.header) == FFX_API_RETURN_OK) { printf("  FSR frame generation in the runtime:"); for (uint64_t k = 0; k < n && k < 8; ++k) printf(" %s", names[k] ? names[k] : "?"); printf("\n"); }
        }
        if (ffxDebug) {   // ffxdebug=1: the runtime's own checks and messages
            ffxConfigureDescGlobalDebug1 dbg{}; dbg.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1; dbg.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;
            dbg.fpMessage = [](uint32_t type, const wchar_t* message) { printf("  FidelityFX %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message); };
            fx.Configure(nullptr, &dbg.header);
        }
        if (!direct) {   // (direct=1: no swap chain at all)
            ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 scd{}; scd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
            scd.swapchain = &chain; scd.hwnd = hwnd; scd.desc = &sd; scd.fullscreenDesc = nullptr; scd.dxgiFactory = factory; scd.gameQueue = g.queue;
            if (const ffxReturnCode_t rc = fx.CreateContext(&chainCtx, &scd.header, nullptr); rc != FFX_API_RETURN_OK || !chain) { printf("FidelityFX's swap chain could not be made (code %u)\n", rc); return 4; }
        }
        ffxCreateBackendDX12Desc backend{}; backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; backend.device = g.dev;
        ffxCreateContextDescFrameGeneration create{}; create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION; create.header.pNext = &backend.header;
        create.flags = (depthDir.empty() && !depthFromMotion && !depthNoise && !depthLayer && !depthOrbit) || !depthNearIsOne ? 0u : static_cast<uint32_t>(FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED);
        if (ffxDebug) create.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
        create.displaySize = { W, H }; create.maxRenderSize = { W, H }; create.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        if (fx.CreateContext(&ctx, &create.header, nullptr) != FFX_API_RETURN_OK || !ctx) { printf("FSR frame generation could not make its context\n"); return 4; }
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain = chain; cfg.frameGenerationEnabled = true; cfg.allowAsyncWorkloads = false; cfg.onlyPresentGenerated = false;
        cfg.generationRect = { 0, 0, static_cast<int>(W), static_cast<int>(H) };
        if (debugView) cfg.flags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW;   // debugview=1: FSR's own views of what it was given, in the frame it makes
        // the generation callback: the runtime makes the frame, then it is copied into the readback buffer on the same command list
        cfg.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* params, void* user) -> ffxReturnCode_t {
            const ffxReturnCode_t rc = s_fx->Dispatch(static_cast<ffxContext*>(user), &params->header);
            auto* list = static_cast<ID3D12GraphicsCommandList*>(params->commandList);
            auto* out = static_cast<ID3D12Resource*>(params->outputs[0].resource);
            if (rc == FFX_API_RETURN_OK && list && out) {
                const uint32_t st = params->outputs[0].state;
                const D3D12_RESOURCE_STATES was = st & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                : st & FFX_API_RESOURCE_STATE_COPY_DEST ? D3D12_RESOURCE_STATE_COPY_DEST
                                                : st & FFX_API_RESOURCE_STATE_COMPUTE_READ ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;
                D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = out;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = was; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                list->ResourceBarrier(1, &b);
                D3D12_TEXTURE_COPY_LOCATION to{ s_readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; to.PlacedFootprint = s_fp;
                D3D12_TEXTURE_COPY_LOCATION from{ out, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; from.SubresourceIndex = 0;
                list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                std::swap(b.Transition.StateBefore, b.Transition.StateAfter); list->ResourceBarrier(1, &b);
                InterlockedIncrement(&s_generated);
            }
            return rc;
        };
        cfg.frameGenerationCallbackUserContext = &ctx;
        if (direct) {   // FSR's own dispatch, on our command list: no swap chain, no present, no pacing (FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY)
            cfg.swapChain = nullptr; cfg.frameGenerationCallback = nullptr; cfg.frameGenerationCallbackUserContext = nullptr;
            cfg.flags |= FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        }
        if (const ffxReturnCode_t rc = fx.Configure(&ctx, &cfg.header); rc != FFX_API_RETURN_OK) { printf("FSR frame generation could not be configured (code %u)\n", rc); return 4; }
        uint64_t frameId = 0;
        if (useEngine) {   // engine=1: the addon's own FgEngine, given the frames through shared textures and fences as framegen11 gives them
            static nr::FgEngine eng;
            DXGI_ADAPTER_DESC1 ad{}; { IDXGIFactory6* f6 = nullptr; CreateDXGIFactory2(0, IID_PPV_ARGS(&f6)); IDXGIAdapter1* a = nullptr; f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)); a->GetDesc1(&ad); a->Release(); f6->Release(); }
            std::wstring dll = Wide(ArgText(argc, argv, "fsr").c_str()); if (dll.empty()) dll = exeDir + L"\\fsr\\amd_fidelityfx_dx12.dll";
            if (!eng.Init(ad.AdapterLuid, dll, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, false, [](const char* m) { printf("  %s\n", m); })) { printf("FgEngine: %s\n", eng.LastError()); return 4; }
            auto sharedTex = [&](ID3D12Resource*& mine, ID3D12Resource*& theirs) {
                D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = W; d.Height = H; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
                d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
                g.dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mine));
                HANDLE h = nullptr; g.dev->CreateSharedHandle(mine, nullptr, GENERIC_ALL, nullptr, &h); theirs = eng.OpenSharedTexture(h); CloseHandle(h);
            };
            auto sharedFence = [&](ID3D12Fence*& mine, ID3D12Fence*& theirs) {
                g.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&mine));
                HANDLE h = nullptr; g.dev->CreateSharedHandle(mine, nullptr, GENERIC_ALL, nullptr, &h); theirs = eng.OpenSharedFence(h); CloseHandle(h);
            };
            static ID3D12Resource *inMine = nullptr, *inTheirs = nullptr, *outMine = nullptr, *outTheirs = nullptr;
            static ID3D12Fence *copiedMine = nullptr, *copiedTheirs = nullptr, *madeMine = nullptr, *madeTheirs = nullptr;
            sharedTex(inMine, inTheirs); sharedTex(outMine, outTheirs); sharedFence(copiedMine, copiedTheirs); sharedFence(madeMine, madeTheirs);
            if (!inTheirs || !outTheirs || !copiedTheirs || !madeTheirs) { printf("FgEngine: the shared resources could not be made\n"); return 4; }
            step = [&](int i, float frameMs, std::vector<uint8_t>& picture) -> bool {
                const uint64_t v = static_cast<uint64_t>(i) + 1;
                g.Barrier(cur, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
                g.list->CopyResource(inMine, cur);   // (a shared texture in COMMON takes a copy without a barrier: simultaneous access)
                g.Barrier(cur, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                g.Submit(); g.queue->Signal(copiedMine, v);
                const bool made = eng.Generate(inTheirs, copiedTheirs, v, outTheirs, madeTheirs, v, frameMs, i == 0);
                g.queue->Wait(madeMine, v);
                if (!made) return true;
                g.Begin();
                D3D12_TEXTURE_COPY_LOCATION to{ readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; to.PlacedFootprint = fp;
                D3D12_TEXTURE_COPY_LOCATION from{ outMine, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; from.SubresourceIndex = 0;
                g.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                g.Submit();
                readPicture(readback, picture);
                if (i >= 2 && i % 20 == 0) printf("  FgEngine: the GPU %.2f ms a frame between\n", eng.GpuMs());
                return true;
            };
        } else if (direct) {
            static ID3D12Resource* made = nullptr;   // the frame FSR makes (unordered access, the frame's format)
            made = g.Texture(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            step = [&, frameId](int i, float frameMs, std::vector<uint8_t>& picture) mutable -> bool {
                cfg.frameID = frameId;   // configure, prepare and dispatch with the same frame ID (else FSR resets)
                if (const ffxReturnCode_t rc = fx.Configure(&ctx, &cfg.header); rc != FFX_API_RETURN_OK) { printf("FSR's configure failed (code %u)\n", rc); return false; }
                ffxDispatchDescFrameGenerationPrepare prep{}; prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
                prep.frameID = frameId; prep.commandList = g.list; prep.renderSize = { W, H }; prep.jitterOffset = { 0, 0 }; prep.motionVectorScale = { mvScale, mvScale };
                prep.frameTimeDelta = frameMs; prep.cameraNear = 0.1f; prep.cameraFar = 1000.0f; prep.cameraFovAngleVertical = 1.0f; prep.viewSpaceToMetersFactor = 1.0f;
                prep.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
                prep.motionVectors = ffxApiGetResourceDX12(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
                if (const ffxReturnCode_t rp = fx.Dispatch(&ctx, &prep.header); rp != FFX_API_RETURN_OK) { printf("FSR's prepare failed (code %u)\n", rp); return false; }
                ffxDispatchDescFrameGeneration gen{}; gen.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
                gen.commandList = g.list; gen.presentColor = ffxApiGetResourceDX12(cur, FFX_API_RESOURCE_STATE_COMPUTE_READ);
                gen.outputs[0] = ffxApiGetResourceDX12(made, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS); gen.numGeneratedFrames = 1; gen.reset = i == 0;
                gen.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB; gen.minMaxLuminance[0] = 0.0f; gen.minMaxLuminance[1] = 1000.0f;
                gen.generationRect = { 0, 0, static_cast<int>(W), static_cast<int>(H) }; gen.frameID = frameId;
                if (const ffxReturnCode_t rg = fx.Dispatch(&ctx, &gen.header); rg != FFX_API_RETURN_OK) { printf("FSR's frame generation dispatch failed (code %u)\n", rg); return false; }
                g.Barrier(made, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION to{ readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; to.PlacedFootprint = fp;
                D3D12_TEXTURE_COPY_LOCATION from{ made, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; from.SubresourceIndex = 0;
                g.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                g.Barrier(made, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                g.Submit();
                ++frameId;
                if (i < 2) return true;
                readPicture(readback, picture);
                return true;
            };
        } else
        step = [&, frameId](int i, float frameMs, std::vector<uint8_t>& picture) mutable -> bool {
            ffxDispatchDescFrameGenerationPrepare prep{}; prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
            prep.frameID = frameId; prep.commandList = g.list; prep.renderSize = { W, H }; prep.jitterOffset = { 0, 0 }; prep.motionVectorScale = { mvScale, mvScale };
            prep.frameTimeDelta = frameMs; prep.cameraNear = 0.1f; prep.cameraFar = 1000.0f; prep.cameraFovAngleVertical = 1.0f; prep.viewSpaceToMetersFactor = 1.0f;
            prep.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
            prep.motionVectors = ffxApiGetResourceDX12(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
            if (const ffxReturnCode_t rp = fx.Dispatch(&ctx, &prep.header); rp != FFX_API_RETURN_OK) { printf("FSR's prepare failed (code %u)\n", rp); return false; }
            ID3D12Resource* back = nullptr; chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back));
            frameIntoBackBuffer(back);
            g.Submit(); back->Release();
            // a mark in the readback buffer, gone once the generated frame has been copied over it
            const uint32_t kMark = 0x5A17C0DEu;
            auto marked = [&](bool set) {
                uint8_t* m = nullptr; D3D12_RANGE all{ 0, static_cast<SIZE_T>(total) }; readback->Map(0, &all, reinterpret_cast<void**>(&m));
                uint32_t* a = reinterpret_cast<uint32_t*>(m + fp.Offset); uint32_t* z = reinterpret_cast<uint32_t*>(m + fp.Offset + static_cast<size_t>(H - 1) * fp.Footprint.RowPitch);
                bool still = false;
                for (int k = 0; k < 8; ++k) { if (set) a[k] = z[k] = kMark; else still = still || a[k] == kMark || z[k] == kMark; }
                readback->Unmap(0, set ? &all : nullptr);
                return still;
            };
            marked(true);
            cfg.frameID = frameId++;
            fx.Configure(&ctx, &cfg.header);
            const LONG before = s_generated;
            if (FAILED(chain->Present(0, 0))) { printf("present failed\n"); return false; }
            if (i < 2) { Sleep(50); return true; }
            const ULONGLONG t0 = GetTickCount64();
            while ((s_generated == before || marked(false)) && GetTickCount64() - t0 < 3000) Sleep(1);
            if (s_generated == before || marked(false)) { printf("no generated frame came within 3 s\n"); return false; }
            readPicture(readback, picture);
            return true;
        };
    } else {
#if NR_HAVE_XEFG
        std::wstring dll = Wide(ArgText(argc, argv, "xessfg").c_str());
        if (dll.empty()) dll = exeDir + L"\\xess\\libxess_fg.dll";
        xefgModule = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!xefgModule) { printf("Intel's frame generation runtime could not be loaded from %ls\n", dll.c_str()); return 4; }
        auto get = [&](auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(xefgModule, name)); return fn != nullptr; };
        decltype(&xefgSwapChainD3D12CreateContext) xCreate = nullptr; decltype(&xefgSwapChainD3D12InitFromSwapChain) xInit = nullptr;
        decltype(&xefgSwapChainD3D12GetSwapChainPtr) xPtr = nullptr; decltype(&xefgSwapChainSetEnabled) xEnable = nullptr;
        if (!get(xCreate, "xefgSwapChainD3D12CreateContext") || !get(xInit, "xefgSwapChainD3D12InitFromSwapChain") || !get(xPtr, "xefgSwapChainD3D12GetSwapChainPtr") ||
            !get(xEnable, "xefgSwapChainSetEnabled") || !get(xTag, "xefgSwapChainD3D12TagFrameResource") || !get(xConst, "xefgSwapChainTagFrameConstants") ||
            !get(xPresentId, "xefgSwapChainSetPresentId") || !get(xDestroy, "xefgSwapChainDestroy")) { printf("Intel's runtime lacks the frame generation API\n"); return 4; }
        IDXGISwapChain1* real1 = nullptr;
        if (FAILED(factory->CreateSwapChainForHwnd(g.queue, hwnd, &sd, nullptr, nullptr, &real1))) { printf("the swap chain could not be made\n"); return 4; }
        IDXGISwapChain3* real = nullptr; real1->QueryInterface(IID_PPV_ARGS(&real)); real1->Release();
        // the Present hook and the capture ring
        InitializeCriticalSection(&g_cap.lock);
        g_cap.chain = real; g_cap.dev = g.dev; g_cap.queue = g.queue; g_cap.fp = fp;
        g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_cap.alloc));
        g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_cap.alloc, nullptr, IID_PPV_ARGS(&g_cap.list)); g_cap.list->Close();
        g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_cap.fence));
        for (auto& r : g_cap.ring) r = g.Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        void** vtable = *reinterpret_cast<void***>(real);
        PatchSlot(vtable, 8, reinterpret_cast<void*>(&HookedPresent), reinterpret_cast<void**>(&g_cap.present));     // IDXGISwapChain::Present
        PatchSlot(vtable, 22, reinterpret_cast<void*>(&HookedPresent1), reinterpret_cast<void**>(&g_cap.present1));   // IDXGISwapChain1::Present1
        if (xCreate(g.dev, &xefg) != XEFG_SWAPCHAIN_RESULT_SUCCESS || !xefg) { printf("XeSS frame generation could not make its context\n"); return 4; }
        {
            xellModule = LoadLibraryExW((exeDir + L"\\xess\\libxell.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            decltype(&xellD3D12CreateContext) llCreate = nullptr; decltype(&xefgSwapChainSetLatencyReduction) xLatency = nullptr;
            if (!xellModule) { printf("Intel's XeLL runtime could not be loaded (xess\\libxell.dll next to this tool)\n"); return 4; }
            auto getLl = [&](auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(xellModule, name)); return fn != nullptr; };
            if (!getLl(llCreate, "xellD3D12CreateContext") || !getLl(llMark, "xellAddMarkerData") || !getLl(llDestroy, "xellDestroyContext") ||
                !get(xLatency, "xefgSwapChainSetLatencyReduction")) { printf("Intel's runtimes lack the XeLL API\n"); return 4; }
            if (llCreate(g.dev, &xell) != XELL_RESULT_SUCCESS || !xell) { printf("XeLL could not make its context\n"); return 4; }
            if (xLatency(xefg, xell) != XEFG_SWAPCHAIN_RESULT_SUCCESS) { printf("XeSS frame generation did not take XeLL\n"); return 4; }
            decltype(&xellSetSleepMode) llSleep = nullptr;   // low-latency mode on, as Intel's sample has it
            if (getLl(llSleep, "xellSetSleepMode")) { xell_sleep_params_t sp{}; sp.bLowLatencyMode = 1; llSleep(xell, &sp); }
        }
        // its shaders built now, not at the first frames
        { decltype(&xefgSwapChainD3D12BuildPipelines) xBuild = nullptr;
          if (get(xBuild, "xefgSwapChainD3D12BuildPipelines")) printf("  XeSS frame generation: pipelines built (code %d)\n", static_cast<int>(xBuild(xefg, nullptr, 1, XEFG_SWAPCHAIN_INIT_FLAG_NONE))); }
        get(xStatus, "xefgSwapChainGetLastPresentStatus");
        xefg_swapchain_d3d12_init_params_t ip{}; ip.pApplicationSwapChain = real; ip.initFlags = depthDir.empty() || !depthNearIsOne ? XEFG_SWAPCHAIN_INIT_FLAG_NONE : XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH; ip.maxInterpolatedFrames = 1;
        ip.creationNodeMask = 1; ip.visibleNodeMask = 1; ip.uiMode = XEFG_SWAPCHAIN_UI_MODE_NONE;
        if (const xefg_swapchain_result_t rc = xInit(xefg, g.queue, &ip); rc != XEFG_SWAPCHAIN_RESULT_SUCCESS) { printf("XeSS frame generation could not start (code %d)\n", static_cast<int>(rc)); return 4; }
        if (xPtr(xefg, IID_PPV_ARGS(&proxy)) != XEFG_SWAPCHAIN_RESULT_SUCCESS || !proxy) { printf("XeSS frame generation gave no swap chain\n"); return 4; }
        xEnable(xefg, 1);
        uint32_t presentId = 0;
        step = [&, presentId](int i, float frameMs, std::vector<uint8_t>& picture) mutable -> bool {
            ++presentId;
            llMark(xell, presentId, XELL_SIMULATION_START); llMark(xell, presentId, XELL_SIMULATION_END);
            llMark(xell, presentId, XELL_RENDERSUBMIT_START);
            auto tag = [&](xefg_swapchain_resource_type_t type, ID3D12Resource* r) {
                xefg_swapchain_d3d12_resource_data_t d{}; d.type = type; d.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
                d.resourceBase = { 0, 0 }; d.resourceSize = { W, H }; d.pResource = r; d.incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                return xTag(xefg, g.list, presentId, &d) == XEFG_SWAPCHAIN_RESULT_SUCCESS;
            };
            if (!tag(XEFG_SWAPCHAIN_RES_HUDLESS_COLOR, cur) || !tag(XEFG_SWAPCHAIN_RES_DEPTH, depth) || !tag(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, motion)) { printf("XeSS frame generation did not take the frame's resources\n"); return false; }
            xefg_swapchain_frame_constant_data_t c{};   // no camera: identity view and projection; motion in the frame's pixels
            for (int k = 0; k < 4; ++k) { c.viewMatrix[k * 5] = 1.0f; c.projectionMatrix[k * 5] = 1.0f; }
            c.motionVectorScaleX = 1.0f; c.motionVectorScaleY = 1.0f; c.resetHistory = i == 0 ? 1u : 0u; c.frameRenderTime = frameMs;
            xConst(xefg, presentId, &c);
            ID3D12Resource* back = nullptr; proxy->GetBuffer(proxy->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back));
            frameIntoBackBuffer(back);
            g.Submit(); back->Release();
            llMark(xell, presentId, XELL_RENDERSUBMIT_END);
            xPresentId(xefg, presentId);
            const LONG before = g_cap.count;
            llMark(xell, presentId, XELL_PRESENT_START);
            if (FAILED(proxy->Present(0, 0))) { printf("present failed\n"); return false; }
            llMark(xell, presentId, XELL_PRESENT_END);
            if (i < 2) { Sleep(50); return true; }
            // two presents follow: the generated frame, then this one
            const ULONGLONG t0 = GetTickCount64();
            while (g_cap.count < before + 2 && GetTickCount64() - t0 < 3000) Sleep(1);
            if (g_cap.count < before + 2) {
                xefg_swapchain_present_status_t st{}; if (xStatus) xStatus(xefg, &st);
                printf("XeSS presented %ld frame(s) instead of 2 within 3 s (its last present: %u frames, result %d, generation %s; %ld captured in all)\n",
                       g_cap.count - before, st.framesPresented, static_cast<int>(st.frameGenResult), st.isFrameGenEnabled ? "on" : "off", g_cap.count);
                return false;
            }
            EnterCriticalSection(&g_cap.lock); const uint64_t v = g_cap.value; LeaveCriticalSection(&g_cap.lock);
            g_cap.fence->SetEventOnCompletion(v, nullptr);
            readPicture(g_cap.ring[before % PresentCapture::kRing], picture);
            return true;
        };
#else
        printf("built without Intel's frame generation headers (tools\\fetch_xess_sdk.ps1)\n"); return 4;
#endif
    }

    struct Score { int frame; double gen, hold, blend, genAbs, genCoarse, blendCoarse, genBand = 0, blendBand = 0, bandFrac = 0; std::vector<uint8_t> picture, mix; };
    std::vector<Score> scores;
    D3D12_RESOURCE_STATES curState = D3D12_RESOURCE_STATE_COPY_DEST;
    for (int i = 0; i < count; i += stride) {   // the kept frames: 0, 2, 4...; each one's generated frame stands for the one before it (i - 1)
        {   // upload the kept frame
            uint8_t* m = nullptr; upload->Map(0, nullptr, reinterpret_cast<void**>(&m));
            for (uint32_t y = 0; y < H; ++y) memcpy(m + fp.Offset + y * fp.Footprint.RowPitch, frames[i].data() + static_cast<size_t>(y) * W * 4, W * 4);
            upload->Unmap(0, nullptr);
        }
        g.Begin();
        g.Barrier(cur, curState, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to{ cur, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; to.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION from{ upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; from.PlacedFootprint = fp;
        g.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        g.Barrier(cur, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); curState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        // the motion from the kept frame before to this one (the estimate keeps the frame before itself), left readable for the generator
        if (estIn.empty()) est.Record(g.list, 0, cur, DXGI_FORMAT_R8G8B8A8_UNORM, motion, distrust);
        else {   // the estimate fed the frame as light (half floats, 1 = the SDR white), as Lossless Scaling's HDR frames reach it live
            static ID3D12Resource* light = nullptr; static ID3D12Resource* lightUp = nullptr; static D3D12_PLACED_SUBRESOURCE_FOOTPRINT lfp{};
            if (!light) {
                light = g.Texture(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_RESOURCE_DESC ld = light->GetDesc(); UINT lr = 0; UINT64 lrow = 0, lt = 0; g.dev->GetCopyableFootprints(&ld, 0, 1, 0, &lfp, &lr, &lrow, &lt);
                lightUp = g.Buffer(lt, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            } else g.Barrier(light, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            uint8_t* m = nullptr; lightUp->Map(0, nullptr, reinterpret_cast<void**>(&m));
            static float table[256]; static bool made = false;
            if (!made) {   // the SDR view back to light: sRGB decoded, then the roll-off above 0.75 undone (hdr_hlsl.h's Expand)
                const float span = std::log(1.0f + 125.0f / 0.03f);
                for (int v = 0; v < 256; ++v) {
                    const float s = v / 255.0f, l = s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
                    table[v] = l <= 0.75f ? l : 0.75f + 0.03f * (std::exp(std::min(l - 0.75f, 0.25f) / 0.25f * span) - 1.0f);
                }
                made = true;
            }
            for (uint32_t y = 0; y < H; ++y) {
                uint16_t* row = reinterpret_cast<uint16_t*>(m + lfp.Offset + y * lfp.Footprint.RowPitch);
                const uint8_t* src = frames[i].data() + static_cast<size_t>(y) * W * 4;
                for (uint32_t x = 0; x < W * 4; ++x) row[x] = DirectX::PackedVector::XMConvertFloatToHalf((x & 3) == 3 ? 1.0f : table[src[x]] * 3.0f);   // scRGB (1 = 80 nits), the SDR white at 240
            }
            lightUp->Unmap(0, nullptr);
            D3D12_TEXTURE_COPY_LOCATION lt{ light, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; lt.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION lf{ lightUp, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; lf.PlacedFootprint = lfp;
            g.list->CopyTextureRegion(&lt, 0, 0, 0, &lf, nullptr);
            g.Barrier(light, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            // estin=light: taken as it is (before the fix); estin=view: in its SDR view (scRGB, the SDR white at 240 nits)
            est.Record(g.list, 0, light, DXGI_FORMAT_R16G16B16A16_FLOAT, motion, distrust, 0.0f, estIn == "view" ? 1u : 0u, 240.0f);
        }
        if (noMotion) {   // overwritten with zeros (a copy from an all-zero texture of the same format)
            static ID3D12Resource* zero = nullptr;
            if (!zero) zero = g.Texture(W, H, DXGI_FORMAT_R16G16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g.Barrier(motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            g.list->CopyResource(motion, zero);
            g.Barrier(motion, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        if (!depthDir.empty()) {
            if (!loadDepth(first + i)) { printf("frame %d: no depth_%05d.bin in the depth folder\n", first + i, first + i); return 3; }
            writeDepth(depthValues.data());
        }
        std::vector<uint8_t> band; double bandFrac = 0;   // where turn ghosting shows (BandMask), for the band score
        std::vector<uint16_t> frameMv;                     // this frame's vectors, for the guard
        if (!refine.empty() || mvCheck || depthFromMotion || depthNoise || depthLayer || depthOrbit || stride == 2 || guardApart > 0) {   // the estimate's vectors to the CPU (refined and back, checked, made into depth, for the band)
            static ID3D12Resource* mvReadback = nullptr; static ID3D12Resource* mvUpload = nullptr; static D3D12_PLACED_SUBRESOURCE_FOOTPRINT mfp{}; static UINT64 mtotal = 0;
            if (!mvReadback) {
                D3D12_RESOURCE_DESC md = motion->GetDesc(); UINT mrows = 0; UINT64 mrow = 0;
                g.dev->GetCopyableFootprints(&md, 0, 1, 0, &mfp, &mrows, &mrow, &mtotal);
                mvReadback = g.Buffer(mtotal, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
                mvUpload = g.Buffer(mtotal, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            }
            g.Barrier(motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION rt{ mvReadback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; rt.PlacedFootprint = mfp;
            D3D12_TEXTURE_COPY_LOCATION rf{ motion, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; rf.SubresourceIndex = 0;
            g.list->CopyTextureRegion(&rt, 0, 0, 0, &rf, nullptr);
            g.Submit();
            std::vector<uint16_t> mv(static_cast<size_t>(W) * H * 2);
            { uint8_t* m = nullptr; mvReadback->Map(0, nullptr, reinterpret_cast<void**>(&m));
              for (uint32_t y = 0; y < H; ++y) memcpy(mv.data() + static_cast<size_t>(y) * W * 2, m + mfp.Offset + y * mfp.Footprint.RowPitch, W * 4);
              D3D12_RANGE none{ 0, 0 }; mvReadback->Unmap(0, &none); }
            if (mvCheck && i >= 2) {
                std::vector<uint8_t> moved, half; double len = 0;
                WarpByMotion(frames[i - 2], mv, W, H, moved, &len);
                // the frame between from the vectors alone: both kept frames moved half way (each pixel's own vector), mixed
                HalfWay(frames[i - 2], frames[i], mv, W, H, half, 0.5f);
                // and where between them the frame between best fits (its time may not be half way: uneven frame pacing)
                float bestAt = 0.5f; double bestDb = Psnr(half, frames[i - 1], nullptr); const double halfDb = bestDb;
                for (float at : { 0.0f, 0.2f, 0.35f, 0.65f, 0.8f, 1.0f }) {
                    HalfWay(frames[i - 2], frames[i], mv, W, H, moved, at);
                    const double db = Psnr(moved, frames[i - 1], nullptr); if (db > bestDb) { bestDb = db; bestAt = at; }
                }
                printf("  frame %4d  vectors: still %5.2f dB, average length %.1f px; half way %5.2f dB against frame %d, best at %.2f (%5.2f dB); times %.1f + %.1f ms\n",
                       first + i, Psnr(frames[i - 2], frames[i], nullptr), len, halfDb, first + i - 1, bestAt, bestDb, times[i - 1] - times[i - 2], times[i] - times[i - 1]);
                for (int reach : { 8, 24 }) {
                    MidMatch(frames[i - 2], frames[i], mv, W, H, moved, reach);
                    printf("  frame %4d  matched at its own time (reach %d px): %5.2f dB, coarse %5.2f dB (half way coarse %5.2f dB)\n", first + i - 1, reach,
                           Psnr(moved, frames[i - 1], nullptr), PsnrCoarse(moved, frames[i - 1], W), PsnrCoarse(half, frames[i - 1], W));
                    if (reach == 8) { static int saved = 0; if (saved < 2 && len > 40) { wchar_t name[64]; swprintf(name, 64, L"\\mid_frame%05d.bmp", first + i - 1); WriteBmp(outDir + name, { &frames[i - 1], &moved, &half }, W, H); ++saved; } }
                }
            }
            if (!refine.empty()) RefineMotion(mv, frames[i], depthValues, W, H, sigmaDepth, sigmaColour);
            { uint8_t* m = nullptr; mvUpload->Map(0, nullptr, reinterpret_cast<void**>(&m));
              for (uint32_t y = 0; y < H; ++y) memcpy(m + mfp.Offset + y * mfp.Footprint.RowPitch, mv.data() + static_cast<size_t>(y) * W * 2, W * 4);
              mvUpload->Unmap(0, nullptr); }
            g.Begin();
            g.Barrier(motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION ut{ motion, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; ut.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION uf{ mvUpload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; uf.PlacedFootprint = mfp;
            g.list->CopyTextureRegion(&ut, 0, 0, 0, &uf, nullptr);
            g.Barrier(motion, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (depthFromMotion) { DepthFromMotion(mv, W, H, depthValues, depthPx); writeDepth(depthValues.data()); }
            if (depthNoise) {   // depth=noise: random depth (does the generator read depth at all?)
                depthValues.resize(static_cast<size_t>(W) * H); uint32_t r = 0x9E3779B9u ^ static_cast<uint32_t>(i);
                for (float& d : depthValues) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; d = 0.1f + 0.9f * (r & 0xFFFF) / 65535.0f; }
                writeDepth(depthValues.data());
            }
            if (depthLayer) { DepthLayers(mv, W, H, depthValues, layerNeighbours); writeDepth(depthValues.data()); }
            if (depthOrbit) { DepthOrbit(mv, W, H, depthValues); writeDepth(depthValues.data()); }
            bandFrac = BandMask(mv, W, H, band);
            if (guardApart > 0) frameMv = mv;
            if (mvDump && i >= 2) {   // mvdump=1: the vectors as a picture beside the frame (red: x, green: y; 1 level a pixel, 128 = none)
                std::vector<uint8_t> pic(static_cast<size_t>(W) * H * 4);
                for (size_t p = 0; p < static_cast<size_t>(W) * H; ++p) {
                    const float vx = DirectX::PackedVector::XMConvertHalfToFloat(mv[p * 2]), vy = DirectX::PackedVector::XMConvertHalfToFloat(mv[p * 2 + 1]);
                    pic[p * 4] = static_cast<uint8_t>(std::clamp(128.0f + vx, 0.0f, 255.0f)); pic[p * 4 + 1] = static_cast<uint8_t>(std::clamp(128.0f + vy, 0.0f, 255.0f));
                    pic[p * 4 + 2] = 128; pic[p * 4 + 3] = 255;
                }
                wchar_t name[64]; swprintf(name, 64, L"\\vectors_%05d.bmp", first + i);
                WriteBmp(outDir + name, { &frames[i], &pic }, W, H);
            }
        }
        g.Barrier(motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Score s; s.frame = first + i - 1;
        const float frameMs = i >= stride ? static_cast<float>(times[i] - times[i - stride]) : 33.3f;
        LARGE_INTEGER s0, s1, sf; QueryPerformanceCounter(&s0);
        const bool ok = step(i, frameMs, s.picture);
        QueryPerformanceCounter(&s1); QueryPerformanceFrequency(&sf);
        static double stepSum = 0; static int stepCount = 0;
        if (i >= 2 * stride) { stepSum += (s1.QuadPart - s0.QuadPart) * 1000.0 / sf.QuadPart; ++stepCount; }
        if (i + stride >= count && stepCount) printf("  making a frame between took %.2f ms on average (the thread's wait, %d frames)\n", stepSum / stepCount, stepCount);
        g.Begin(); g.Barrier(motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); g.Submit();
        if (!ok) { printf("frame %d: stopped\n", first + i); return 5; }
        est.ReadStats(0);   // the estimate's average motion over this frame (the GPU has finished it)
        double mx = 0, my = 0, mlen = 0, mcost = 0, mdis = 0; uint64_t mframes = 0; est.TakeAverages(mx, my, mlen, mcost, mdis, mframes);
        if (stride == 1) {   // live: nothing to score against; the frame between is kept as a picture (the frame before | the generator's | this frame)
            if (i >= 2) {
                wchar_t name[64]; swprintf(name, 64, L"\\live_%05d_%05d.bmp", first + i - 1, first + i);
                if (guardApart > 0) {   // (the frame before | the generator's | guarded | this frame)
                    std::vector<uint8_t> guarded = s.picture;
                    const double share = Guard(guarded, frames[i - 1], frames[i], W, H, guardRadius, static_cast<float>(guardAgree), static_cast<float>(guardApart), frameMv);
                    WriteBmp(outDir + name, { &frames[i - 1], &s.picture, &guarded, &frames[i] }, W, H);
                    printf("  between frames %d and %d: %ls (guard replaced %.2f%%)\n", first + i - 1, first + i, (outDir + name).c_str(), 100.0 * share);
                } else {
                    WriteBmp(outDir + name, { &frames[i - 1], &s.picture, &frames[i] }, W, H);
                    printf("  between frames %d and %d: %ls\n", first + i - 1, first + i, (outDir + name).c_str());
                }
            }
            continue;
        }
        if (i < 2) continue;   // the first kept frame has no frame before it
        if (keepStill > 0) KeepStill(s.picture, frames[i - 2], frames[i], W, H, static_cast<float>(keepStill));
        if (guardApart > 0) Guard(s.picture, frames[i - 2], frames[i], W, H, guardRadius, static_cast<float>(guardAgree), static_cast<float>(guardApart), frameMv);
        const std::vector<uint8_t>& truth = frames[i - 1];
        s.mix.resize(truth.size());
        for (size_t k = 0; k < truth.size(); ++k) s.mix[k] = static_cast<uint8_t>((frames[i - 2][k] + frames[i][k] + 1) / 2);
        s.gen = Psnr(s.picture, truth, &s.genAbs); s.hold = Psnr(frames[i - 2], truth, nullptr); s.blend = Psnr(s.mix, truth, nullptr);
        s.genCoarse = PsnrCoarse(s.picture, truth, W); s.blendCoarse = PsnrCoarse(s.mix, truth, W);
        s.bandFrac = bandFrac;
        if (bandFrac > 0.002) { s.genBand = PsnrMasked(s.picture, truth, band); s.blendBand = PsnrMasked(s.mix, truth, band); }
        const double same = Psnr(s.picture, frames[i], nullptr);
        printf("  frame %4d  %s %5.2f dB (%.2f levels off; coarse %5.2f dB)   hold %5.2f dB   blend %5.2f dB (coarse %5.2f dB)   motion %4.0f px (%+.0f, %+.0f)%s\n", s.frame, G, s.gen, s.genAbs, s.genCoarse, s.hold, s.blend, s.blendCoarse, mlen, mx, my,
               same > 60.0 ? "   (the generator's frame is the kept frame itself)" : "");
        if (s.bandFrac > 0.002) printf("  frame %4d  band (%.1f%% of the picture): %s %5.2f dB, blend %5.2f dB\n", s.frame, 100.0 * s.bandFrac, G, s.genBand, s.blendBand);
        scores.push_back(std::move(s));
    }
    if (stride == 1) return 0;
    if (scores.empty()) { printf("nothing was rebuilt\n"); return 5; }
    double gen = 0, hold = 0, blend = 0; int better = 0;
    double genC = 0, blendC = 0;
    for (const Score& s : scores) { gen += s.gen; hold += s.hold; blend += s.blend; genC += s.genCoarse; blendC += s.blendCoarse; better += s.gen > std::max(s.hold, s.blend) ? 1 : 0; }
    const double n = static_cast<double>(scores.size());
    printf("average over %zu rebuilt frames: %s %.2f dB, hold %.2f dB, blend %.2f dB; %s the closest in %d of them; coarse: %s %.2f dB, blend %.2f dB\n", scores.size(), G, gen / n, hold / n, blend / n, G, better, G, genC / n, blendC / n);
    { double gb = 0, bb = 0; int nb = 0; for (const Score& s : scores) if (s.bandFrac > 0.002) { gb += s.genBand; bb += s.blendBand; ++nb; }
      if (nb) printf("band (around what moves unlike the picture, over %d turning frames): %s %.2f dB, blend %.2f dB\n", nb, G, gb / nb, bb / nb); }
    std::vector<const Score*> order; for (const Score& s : scores) order.push_back(&s);
    // the worst by the whole picture, or (worstby=band) by the band, frames without one last
    auto key = [&](const Score* s) { return !worstByBand ? s->gen : s->bandFrac > 0.002 ? s->genBand : 1e9; };
    std::sort(order.begin(), order.end(), [&](const Score* a, const Score* b) { return key(a) < key(b); });
    for (int k = 0; k < worstShown && k < static_cast<int>(order.size()); ++k) {
        const Score& s = *order[k];
        wchar_t name[64]; swprintf(name, 64, L"\\worst%d_frame%05d.bmp", k + 1, s.frame);
        WriteBmp(outDir + name, { &frames[s.frame - first], &s.picture, &s.mix }, W, H);
        printf("  worst %d: frame %d, %s %.2f dB: %ls (the real frame | %s | the blend)\n", k + 1, s.frame, G, s.gen, (outDir + name).c_str(), G);
    }
    if (ctx) fx.DestroyContext(&ctx, nullptr);
#if NR_HAVE_XEFG
    if (xefg && xDestroy) xDestroy(xefg);
    if (xell && llDestroy) llDestroy(xell);
#endif
    return 0;
}
