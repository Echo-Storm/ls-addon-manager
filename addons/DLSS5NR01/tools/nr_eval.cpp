// nr_nreval: runs Neural Rendering's model (the addon's own NrEngine, with the user's nvngx_dlssnr.dll) on a recording and scores the flicker it adds.
//
//   nr_nreval <recording.lsrec> <output folder> [first=N] [count=N] [scale=50] [smooth=40] [passes=1] [intensity=100] [model=<path to nvngx_dlssnr.dll>]
//             [lsdir=<Lossless Scaling folder>] [show=N] [maxdelta=50] [still=3]
//
// The picture shown is the game's frame plus the model's change (its "delta", at the working size, stretched to the frame's and clamped to
// maxdelta percent), as the addon's compose adds it. Per frame, against the frame before:
//   * steady: the picture's change from the last one against the game's own change (dB; the score nr_sreval prints): what the model adds
//     to the flicker, plus what it takes away;
//   * along the motion: the delta against the one before it, moved along the frames' own motion (block matching): the flicker in what moves;
//   * still: over the pixels where the game's frame did not change (within `still` levels), the mean change of the model's delta, in levels
//     of 255: pure model flicker, since nothing moved there. This is the number to bring down.
// The engine is driven the way the addon drives it (shared textures and fences, the engine's own motion estimate and delta smoothing).
#include <windows.h>
#include <shlobj.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "addon/lsrec.h"
#include "engine/nr_engine.h"
#include "forwarder/nr_api.h"
#include "eval_common.h"

using namespace nr::eval;

namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 31u, man = h & 1023u;
    float f;
    if (exp == 0) f = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31) f = man ? NAN : INFINITY;
    else f = std::ldexp(static_cast<float>(man + 1024), static_cast<int>(exp) - 25);
    return sign ? -f : f;
}

// the delta (dw x dh floats, 3 channels) at (x, y) of a w x h picture, bilinear, pixel centres aligned
void SampleDelta(const std::vector<float>& d, uint32_t dw, uint32_t dh, uint32_t x, uint32_t y, uint32_t w, uint32_t h, float out[3]) {
    const float fx = std::clamp((x + 0.5f) * dw / w - 0.5f, 0.0f, dw - 1.0f), fy = std::clamp((y + 0.5f) * dh / h - 0.5f, 0.0f, dh - 1.0f);
    const uint32_t x0 = static_cast<uint32_t>(fx), y0 = static_cast<uint32_t>(fy), x1 = std::min(x0 + 1, dw - 1), y1 = std::min(y0 + 1, dh - 1);
    const float ax = fx - x0, ay = fy - y0;
    for (int c = 0; c < 3; ++c) {
        const float a = d[(static_cast<size_t>(y0) * dw + x0) * 4 + c], b = d[(static_cast<size_t>(y0) * dw + x1) * 4 + c];
        const float e = d[(static_cast<size_t>(y1) * dw + x0) * 4 + c], f = d[(static_cast<size_t>(y1) * dw + x1) * 4 + c];
        out[c] = (a + (b - a) * ax) * (1 - ay) + (e + (f - e) * ax) * ay;
    }
}

// The model's change against the one before it moved along the real motion of the game's frames: the frames are matched block by block (8x8
// blocks of a quarter-size luma, +-8 pixels), the previous delta is taken from where each block came from, and what is left is the flicker in
// what moves. Blocks that do not match well (something came into view, or a thin repeating pattern) are left out. Returns the mean change in levels
// of 255 (over the blocks used), and the share of the picture that was.
double MovingFlicker(const std::vector<uint8_t>& frame, const std::vector<uint8_t>& framePrev, uint32_t W, uint32_t H,
                     const std::vector<float>& d, const std::vector<float>& dPrev, uint32_t dw, uint32_t dh, double* used, double* p95 = nullptr, std::vector<uint8_t>* heat = nullptr) {
    const uint32_t lw = W / 4, lh = H / 4;
    auto luma = [&](const std::vector<uint8_t>& f, std::vector<float>& out) {
        out.assign(static_cast<size_t>(lw) * lh, 0.0f);
        for (uint32_t y = 0; y < lh; ++y) for (uint32_t x = 0; x < lw; ++x) {
            float sum = 0;
            for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) {
                const uint8_t* p = f.data() + (static_cast<size_t>(y * 4 + j) * W + x * 4 + i) * 4;
                sum += 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            }
            out[static_cast<size_t>(y) * lw + x] = sum / 16.0f;
        }
    };
    static std::vector<float> a, b; luma(frame, a); luma(framePrev, b);
    const int B = 8, R = 8;
    double sum = 0; size_t blocks = 0, total = 0;
    std::vector<double> perBlock;
    const uint32_t gx = (lw - 2 * R) / B, gy = (lh - 2 * R) / B;
    if (heat) heat->assign(static_cast<size_t>(gx) * gy, 255);   // (255: not used)
    for (uint32_t by = R; by + B + R <= lh; by += B) for (uint32_t bx = R; bx + B + R <= lw; bx += B) {
        ++total;
        float best = 1e30f; int bsx = 0, bsy = 0;
        for (int sy = -R; sy <= R; ++sy) for (int sx = -R; sx <= R; ++sx) {
            float sad = 0;
            for (int j = 0; j < B; ++j) for (int i = 0; i < B; ++i)
                sad += std::fabs(a[static_cast<size_t>(by + j) * lw + bx + i] - b[static_cast<size_t>(by + j + sy) * lw + bx + i + sx]);
            if (sad < best || (sad == best && std::abs(sx) + std::abs(sy) < std::abs(bsx) + std::abs(bsy))) { best = sad; bsx = sx; bsy = sy; }
        }
        if (best / (B * B) > 4.0f) continue;   // a bad match: not used
        // the block in the delta's pixels (the delta is the frame at dw / W): the same shift, scaled
        const float k = static_cast<float>(dw) / static_cast<float>(W);
        const int x0 = static_cast<int>(bx * 4 * k), y0 = static_cast<int>(by * 4 * k), x1 = static_cast<int>((bx + B) * 4 * k), y1 = static_cast<int>((by + B) * 4 * k);
        const int shx = static_cast<int>(std::lround(bsx * 4 * k)), shy = static_cast<int>(std::lround(bsy * 4 * k));
        double bsum = 0; int cnt = 0;
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
            const int px = x + shx, py = y + shy;
            if (px < 0 || py < 0 || px >= static_cast<int>(dw) || py >= static_cast<int>(dh)) continue;
            double dl = 0;
            for (int c = 0; c < 3; ++c) dl = std::max(dl, double(std::fabs(d[(static_cast<size_t>(y) * dw + x) * 4 + c] - dPrev[(static_cast<size_t>(py) * dw + px) * 4 + c])));
            bsum += dl * 255.0; ++cnt;
        }
        if (cnt) {
            sum += bsum / cnt; ++blocks; perBlock.push_back(bsum / cnt);
            if (heat) (*heat)[static_cast<size_t>((by - R) / B) * gx + (bx - R) / B] = static_cast<uint8_t>(std::min(254.0, bsum / cnt * 20.0));
        }
    }
    if (used) *used = total ? static_cast<double>(blocks) / total : 0.0;
    if (p95) { std::sort(perBlock.begin(), perBlock.end()); *p95 = perBlock.empty() ? 0.0 : perBlock[static_cast<size_t>(perBlock.size() * 0.95)]; }
    return blocks ? sum / blocks : 0.0;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { printf("usage: nr_nreval <recording.lsrec> <output folder> [first=N] [count=N] [scale=50] [smooth=40] [passes=1] [intensity=100] [model=<nvngx_dlssnr.dll>] [lsdir=<folder>] [show=N] [maxdelta=50] [still=3]\n"); return 2; }
    nr::lsrec::Reader rec; std::string error;
    if (!rec.Open(Wide(argv[1]), &error)) { printf("%s: %s\n", argv[1], error.c_str()); return 2; }
    std::wstring outDir = Wide(argv[2]);
    { wchar_t full[MAX_PATH]; if (GetFullPathNameW(outDir.c_str(), MAX_PATH, full, nullptr)) outDir = full; SHCreateDirectoryExW(nullptr, outDir.c_str(), nullptr); }
    const nr::lsrec::FileHeader& h = rec.Header();
    const uint32_t W = h.width, H = h.height;
    const int first = std::max(0, Arg(argc, argv, "first", 0));
    const int count = std::min<int>(Arg(argc, argv, "count", 1 << 30), static_cast<int>(rec.Count()) - first);
    if (count < 2) { printf("the recording has too few frames\n"); return 2; }
    const int show = Arg(argc, argv, "show", 2), stillLevels = Arg(argc, argv, "still", 3);
    const float maxDelta = Arg(argc, argv, "maxdelta", 50) / 100.0f;
    std::wstring exeDir; { wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH); exeDir = exe; exeDir = exeDir.substr(0, exeDir.find_last_of(L'\\')); }
    std::wstring lsDir = ArgText(argc, argv, "lsdir").empty() ? L"D:\\Utilities\\Lossless Scaling" : Wide(ArgText(argc, argv, "lsdir").c_str());
    std::wstring model = ArgText(argc, argv, "model").empty() ? lsDir + L"\\nvngx_dlssnr.dll" : Wide(ArgText(argc, argv, "model").c_str());

    NrParams params;   // the addon's defaults, then what the command line says
    params.workingScale = Arg(argc, argv, "scale", 50) / 100.0f;
    params.deltaSmooth = Arg(argc, argv, "smooth", 40) / 100.0f;
    params.passes = static_cast<uint32_t>(Arg(argc, argv, "passes", 1));
    params.intensity = Arg(argc, argv, "intensity", 100) / 100.0f;
    const float sc = std::clamp(params.workingScale, 0.25f, 1.0f);
    auto workSize = [&](uint32_t full) { uint32_t s = (static_cast<uint32_t>(full * sc) + 7) & ~7u; s = std::min(s, full); return std::max(s, std::min(full, 64u)); };
    const uint32_t dw = workSize(W), dh = workSize(H);

    // our side: a device on the same card, the shared frame in and delta out, three shared fences
    IDXGIFactory6* factory = nullptr; if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return 4;
    IDXGIAdapter1* adapter = nullptr; factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)); factory->Release();
    DXGI_ADAPTER_DESC1 ad{}; adapter->GetDesc1(&ad);
    ID3D12Device* dev = nullptr; if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) { printf("no Direct3D 12 device\n"); return 4; }
    adapter->Release();
    ID3D12CommandQueue* queue = nullptr; ID3D12CommandAllocator* alloc = nullptr; ID3D12GraphicsCommandList* list = nullptr; ID3D12Fence* fence = nullptr; uint64_t fv = 0;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)); dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)); list->Close();
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto submit = [&] { list->Close(); ID3D12CommandList* l[] = { list }; queue->ExecuteCommandLists(1, l); queue->Signal(fence, ++fv); fence->SetEventOnCompletion(fv, event); WaitForSingleObject(event, INFINITE); };
    auto texture = [&](uint32_t tw, uint32_t th, DXGI_FORMAT fmt, bool uav) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = tw; d.Height = th; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
        d.Format = fmt; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE);
        ID3D12Resource* r = nullptr; dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)); return r;
    };
    auto buffer = [&](UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* r = nullptr; dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)); return r;
    };
    ID3D12Resource* in = texture(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, false); ID3D12Resource* delta = texture(dw, dh, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    ID3D12Resource* motion = texture(dw, dh, DXGI_FORMAT_R16G16_FLOAT, true);   // the model's motion vectors, working-size pixels, from this frame to the one before
    ID3D12Fence *copied = nullptr, *used = nullptr, *done = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&copied)); dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&used)); dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&done));
    if (!in || !delta || !copied || !used || !done) { printf("the shared textures or fences could not be made\n"); return 4; }
    D3D12_RESOURCE_DESC inDesc = in->GetDesc(), dDesc = delta->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT inFp{}, dFp{}; UINT rows = 0; UINT64 rowBytes = 0, inTotal = 0, dTotal = 0;
    dev->GetCopyableFootprints(&inDesc, 0, 1, 0, &inFp, &rows, &rowBytes, &inTotal); dev->GetCopyableFootprints(&dDesc, 0, 1, 0, &dFp, &rows, &rowBytes, &dTotal);
    ID3D12Resource* upload = buffer(inTotal, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* readback = buffer(dTotal, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_DESC mDesc = motion->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT mFp{}; UINT64 mTotal = 0;
    dev->GetCopyableFootprints(&mDesc, 0, 1, 0, &mFp, &rows, &rowBytes, &mTotal);
    ID3D12Resource* readbackMotion = buffer(mTotal, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    // the engine, as the addon starts it, given our shared resources
    NrEngine eng;
    eng.SetModel(NrEngine::Model::NeuralRendering, 0);
    if (!eng.Init(ad.AdapterLuid, exeDir + L"\\" NR_FORWARDER_FILENAME, model, exeDir, lsDir, [](const char* m) { printf("  %s\n", m); })) { printf("Neural Rendering could not start (the model %ls)\n", model.c_str()); return 4; }
    for (int i = 0; i < 400 && !eng.Prepare(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, params); ++i) { if (eng.IsFailed()) { printf("the model could not be made\n"); return 4; } Sleep(50); }
    auto share = [&](ID3D12DeviceChild* obj) { HANDLE sh = nullptr; dev->CreateSharedHandle(obj, nullptr, GENERIC_ALL, nullptr, &sh); return sh; };
    ID3D12Resource* inE = eng.OpenSharedTexture(share(in)); ID3D12Resource* deltaE = eng.OpenSharedTexture(share(delta)); ID3D12Resource* motionE = eng.OpenSharedTexture(share(motion));
    ID3D12Fence* copiedE = eng.OpenSharedFence(share(copied)); ID3D12Fence* usedE = eng.OpenSharedFence(share(used)); ID3D12Fence* doneE = eng.OpenSharedFence(share(done));
    if (!inE || !deltaE || !motionE || !copiedE || !usedE || !doneE) { printf("the engine could not open the shared resources\n"); return 4; }
    printf("%ls: %ux%u, %d frames from %d; the model at %ux%u (scale %.2f), smoothing %.2f, %u pass(es)\n", Wide(argv[1]).c_str(), W, H, count, first, dw, dh, params.workingScale, params.deltaSmooth, params.passes);

    struct Score { int frame; double steady, still, moving, p95 = 0, lag, plain; std::vector<uint8_t> src, pic; };
    std::vector<Score> scores;
    std::vector<uint8_t> px, frame, framePrev, pic, picPrev;
    std::vector<float> d, dPrev, mv, mvPrev;
    std::vector<uint8_t> heat; int heatFrame = -1;
    double sumSteady = 0, sumStill = 0, sumMoving = 0, sumP95 = 0, sumLag = 0, sumPlain = 0; int n = 0;
    for (int i = 0; i < count; ++i) {
        if (!rec.Read(first + i, px) || !ToRgba8(h, px, frame)) { printf("frame %d could not be read\n", first + i); return 3; }
        { uint8_t* m = nullptr; upload->Map(0, nullptr, reinterpret_cast<void**>(&m));
          for (uint32_t y = 0; y < H; ++y) memcpy(m + inFp.Offset + y * inFp.Footprint.RowPitch, frame.data() + static_cast<size_t>(y) * W * 4, W * 4);
          upload->Unmap(0, nullptr); }
        alloc->Reset(); list->Reset(alloc, nullptr);
        auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
            D3D12_RESOURCE_BARRIER br{}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b }; list->ResourceBarrier(1, &br);
        };
        barrier(in, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to{ in, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; to.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION from{ upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; from.PlacedFootprint = inFp;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        barrier(in, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        submit();
        queue->Signal(copied, static_cast<uint64_t>(i) + 1);
        queue->Signal(used, static_cast<uint64_t>(i) + 1);   // (no present reads the delta here)
        if (!eng.Prepare(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, params)) { printf("  frame %d: the model is not ready\n", first + i); continue; }
        const bool ran = eng.Run(inE, deltaE, copiedE, static_cast<uint64_t>(i) + 1, usedE, static_cast<uint64_t>(i) + 1, doneE, static_cast<uint64_t>(i) + 1, i == 0, motionE);
        if (!ran) { printf("  frame %d: the model did not run\n", first + i); continue; }
        queue->Wait(done, static_cast<uint64_t>(i) + 1);
        alloc->Reset(); list->Reset(alloc, nullptr);
        barrier(delta, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION rt{ readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; rt.PlacedFootprint = dFp;
        D3D12_TEXTURE_COPY_LOCATION rf{ delta, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; rf.SubresourceIndex = 0;
        list->CopyTextureRegion(&rt, 0, 0, 0, &rf, nullptr);
        barrier(delta, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        barrier(motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION mt{ readbackMotion, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; mt.PlacedFootprint = mFp;
        D3D12_TEXTURE_COPY_LOCATION mf{ motion, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; mf.SubresourceIndex = 0;
        list->CopyTextureRegion(&mt, 0, 0, 0, &mf, nullptr);
        barrier(motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit();
        mv.assign(static_cast<size_t>(dw) * dh * 2, 0.0f);
        { uint8_t* m = nullptr; readbackMotion->Map(0, nullptr, reinterpret_cast<void**>(&m));
          for (uint32_t y = 0; y < dh; ++y) {
              const uint16_t* row = reinterpret_cast<const uint16_t*>(m + mFp.Offset + y * mFp.Footprint.RowPitch);
              for (uint32_t x = 0; x < dw * 2; ++x) mv[static_cast<size_t>(y) * dw * 2 + x] = HalfToFloat(row[x]);
          }
          D3D12_RANGE none{ 0, 0 }; readbackMotion->Unmap(0, &none); }
        d.assign(static_cast<size_t>(dw) * dh * 4, 0.0f);
        { uint8_t* m = nullptr; readback->Map(0, nullptr, reinterpret_cast<void**>(&m));
          for (uint32_t y = 0; y < dh; ++y) {
              const uint16_t* row = reinterpret_cast<const uint16_t*>(m + dFp.Offset + y * dFp.Footprint.RowPitch);
              for (uint32_t x = 0; x < dw * 4; ++x) d[static_cast<size_t>(y) * dw * 4 + x] = HalfToFloat(row[x]);
          }
          D3D12_RANGE none{ 0, 0 }; readback->Unmap(0, &none); }

        // the picture the compose would show: the frame plus the delta (stretched, limited), saturated
        pic.resize(frame.size());
        for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {
            float dd[3]; SampleDelta(d, dw, dh, x, y, W, H, dd);
            const size_t o = (static_cast<size_t>(y) * W + x) * 4;
            for (int c = 0; c < 3; ++c) pic[o + c] = static_cast<uint8_t>(std::clamp(frame[o + c] / 255.0f + std::clamp(dd[c] * params.composeIntensity, -maxDelta, maxDelta), 0.0f, 1.0f) * 255.0f + 0.5f);
            pic[o + 3] = 255;
        }
        Score s; s.frame = first + i; s.steady = 99; s.still = 0; s.moving = 0; s.lag = 0; s.plain = 0; double usedShare = 0;
        if (i > 0 && !framePrev.empty()) {
            s.steady = PsnrTemporal(pic, picPrev, frame, framePrev);
            // the delta's change where the game's frame did not change: model flicker, nothing else
            double sum = 0; size_t cnt = 0;
            for (uint32_t y = 0; y < dh; ++y) for (uint32_t x = 0; x < dw; ++x) {
                const uint32_t fx = std::min(W - 1, static_cast<uint32_t>((x + 0.5) * W / dw)), fy = std::min(H - 1, static_cast<uint32_t>((y + 0.5) * H / dh));
                const size_t o = (static_cast<size_t>(fy) * W + fx) * 4;
                int diff = 0; for (int c = 0; c < 3; ++c) diff = std::max(diff, std::abs(int(frame[o + c]) - int(framePrev[o + c])));
                if (diff > stillLevels) continue;
                const size_t k = (static_cast<size_t>(y) * dw + x) * 4;
                double dl = 0; for (int c = 0; c < 3; ++c) dl = std::max(dl, double(std::abs(d[k + c] - dPrev[k + c])));
                sum += dl * 255.0; ++cnt;
            }
            s.still = cnt > W * H / 100 / 64 ? sum / cnt : 0.0;   // (at least about 1 % of the picture still)
            s.moving = MovingFlicker(frame, framePrev, W, H, d, dPrev, dw, dh, &usedShare, &s.p95, i == count / 2 ? &heat : nullptr);
            if (i == count / 2) heatFrame = first + i;
            // The live path, frame generation off: this frame is shown with the delta of the run before, sampled where the pixel was "one frame back
            // at that run's speed" (compose11: uv + offset * motion / size, the older run's own motion). How far is that from the delta of this
            // frame itself? Against no compensation at all (the delta sampled in place).
            double lagSum = 0, plainSum = 0; size_t lagN = 0;
            for (uint32_t y = 2; y < dh - 2; y += 4) for (uint32_t x = 2; x < dw - 2; x += 4) {
                const size_t k = (static_cast<size_t>(y) * dw + x);
                const float sx = std::clamp(x + 0.5f + mvPrev[k * 2] - 0.5f, 0.0f, dw - 1.0f), sy = std::clamp(y + 0.5f + mvPrev[k * 2 + 1] - 0.5f, 0.0f, dh - 1.0f);
                const uint32_t x0 = static_cast<uint32_t>(sx), y0 = static_cast<uint32_t>(sy), x1 = std::min(x0 + 1, dw - 1), y1 = std::min(y0 + 1, dh - 1);
                const float ax = sx - x0, ay = sy - y0;
                double lagMax = 0, plainMax = 0;
                for (int c = 0; c < 3; ++c) {
                    auto at = [&](uint32_t xx, uint32_t yy) { return dPrev[(static_cast<size_t>(yy) * dw + xx) * 4 + c]; };
                    const float lag = (at(x0, y0) * (1 - ax) + at(x1, y0) * ax) * (1 - ay) + (at(x0, y1) * (1 - ax) + at(x1, y1) * ax) * ay;
                    lagMax = std::max(lagMax, double(std::fabs(lag - d[k * 4 + c])));
                    plainMax = std::max(plainMax, double(std::fabs(dPrev[k * 4 + c] - d[k * 4 + c])));
                }
                lagSum += lagMax * 255.0; plainSum += plainMax * 255.0; ++lagN;
            }
            s.lag = lagSum / lagN; s.plain = plainSum / lagN;
        }
        printf("  frame %4d  steady %5.2f dB   delta change: where the game is still %5.2f levels, along the motion %5.2f levels (%.0f %% of the picture matched)\n", s.frame, s.steady, s.still, s.moving, usedShare * 100.0);
        if (s.steady < 99) printf("             the live path (this frame shown with the run before's delta moved by its motion): %5.2f levels off this frame's own delta; not moved: %5.2f\n", s.lag, s.plain);
        if (i >= 4 && s.steady < 99) { sumSteady += s.steady; sumStill += s.still; sumMoving += s.moving; sumP95 += s.p95; sumLag += s.lag; sumPlain += s.plain; ++n; }
        if (show > 0) { s.src = frame; s.pic = pic; }
        scores.push_back(std::move(s));
        if (show > 0 && scores.size() > 32) {   // keep the pictures of the worst only
            auto best = std::min_element(scores.begin(), scores.end(), [](const Score& a, const Score& b) { return (a.src.empty() ? 1e9 : a.still) < (b.src.empty() ? 1e9 : b.still); });
            best->src.clear(); best->src.shrink_to_fit(); best->pic.clear(); best->pic.shrink_to_fit();
        }
        framePrev = frame; picPrev = pic; dPrev = d; mvPrev = mv;
    }
    if (n) printf("average over %d frames (after the first 4): steady %.2f dB, delta change where the game is still %.2f levels of 255, along the motion %.2f (the worst 5 %% of blocks: %.2f); the live path %.2f (not moved: %.2f)\n", n, sumSteady / n, sumStill / n, sumMoving / n, sumP95 / n, sumLag / n, sumPlain / n);
    if (!heat.empty()) {   // where the flicker sits, frame heatFrame: a block of the frame a pixel, brighter the more the delta changed along the motion (blue: not used)
        const uint32_t lw = W / 4, lh = H / 4, R = 8, B = 8, gx = (lw - 2 * R) / B, gy = (lh - 2 * R) / B, cell = 8;
        std::vector<uint8_t> tile(static_cast<size_t>(gx * cell) * (gy * cell) * 4, 255);
        for (uint32_t y = 0; y < gy * cell; ++y) for (uint32_t x = 0; x < gx * cell; ++x) {
            const uint8_t v = heat[static_cast<size_t>(y / cell) * gx + x / cell];
            uint8_t* p = tile.data() + (static_cast<size_t>(y) * gx * cell + x) * 4;
            if (v == 255) { p[0] = 20; p[1] = 20; p[2] = 90; } else { p[0] = v; p[1] = static_cast<uint8_t>(v / 2); p[2] = 0; }
        }
        WriteBmp(outDir + L"\\flicker_heat.bmp", { &tile }, gx * cell, gy * cell);
        printf("  where the flicker sits (frame %d): %ls\n", heatFrame, (outDir + L"\\flicker_heat.bmp").c_str());
    }
    std::vector<const Score*> order; for (const Score& s : scores) if (!s.src.empty()) order.push_back(&s);
    std::sort(order.begin(), order.end(), [](const Score* a, const Score* b) { return a->still > b->still; });
    for (int k = 0; k < show && k < static_cast<int>(order.size()); ++k) {
        wchar_t name[64]; swprintf(name, 64, L"\\flicker%d_frame%05d.bmp", k + 1, order[k]->frame);
        WriteBmp(outDir + name, { &order[k]->src, &order[k]->pic }, W, H);
        printf("  flickeriest %d: frame %d (%.2f levels): %ls (the frame | with the model)\n", k + 1, order[k]->frame, order[k]->still, (outDir + name).c_str());
    }
    eng.Drain();
    eng.Shutdown();
    return 0;
}
