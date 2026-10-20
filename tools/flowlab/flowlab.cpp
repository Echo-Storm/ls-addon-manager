// flowlab: motion measured two ways on the same recorded frames, written to files for tools/flowlab/score_flow.py.
//
//   flowlab.exe clip=FILE.lsrec start=N count=M out=FOLDER [grid=1|2|4] [perf=slow|medium|fast] [size=1280x720]
//
// The clip's even frames (the real frames a 30 fps game would give; the odd ones are the truth for the frames between) are cropped to size (the middle of the picture).
// For every pair of real frames (A before, C after) the flow AT C pointing back to A is made by
//   ours : this project's flow estimator (src/engine/flow_estimator.cpp, the one under EchoFG and the upscalers), per pixel, in pixels;
//   ofa  : NVIDIA's optical flow hardware (NVOFA) through its public API (nvofapi64.dll, headers MIT, in external/nvof), forward flow from C to A, S10.5 fixed point,
//          one vector per `grid` pixels, made per pixel by repeating each (nearest).
// Both: float32 x, y per pixel (x_in_A - x_in_C), in out\ours.f32 / out\ofa.f32 (one 2 x W x H block per pair), the clip's frames in out\frames.rgba (all, cropped), and the time the
// hardware took per pair is printed. Nothing here is part of a product: it is a lab.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "addon/lsrec.h"
#include "engine/flow_estimator.h"
#include "nvOpticalFlowD3D11.h"

using Microsoft::WRL::ComPtr;

#define CHECK(expr) do { const HRESULT hr_ = (expr); if (FAILED(hr_)) { printf("FAILED %s: 0x%08X\n", #expr, (unsigned)hr_); return 1; } } while (0)
#define OF_CHECK(expr) do { const NV_OF_STATUS s_ = (expr); if (s_ != NV_OF_SUCCESS) { printf("FAILED %s: NV_OF_STATUS %d\n", #expr, (int)s_); return 1; } } while (0)

static float HalfToFloat(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float f;
    if (e == 0) f = std::ldexp((float)m, -24);
    else if (e == 31) f = m ? NAN : INFINITY;
    else f = std::ldexp((float)(m + 1024), (int)e - 25);
    return s ? -f : f;
}

static std::string Arg(int argc, char** argv, const char* key, const char* fallback = "") {
    const size_t k = strlen(key);
    for (int i = 1; i < argc; ++i) if (!strncmp(argv[i], key, k) && argv[i][k] == '=') return argv[i] + k + 1;
    return fallback;
}

int main(int argc, char** argv) {
    const std::string clipPath = Arg(argc, argv, "clip"), outDir = Arg(argc, argv, "out", "."), rawPath = Arg(argc, argv, "rawframes");   // rawframes=FILE: frames in the layout flowlab writes (a made-up scene, tools/flowlab/make_synth_clip.py) in place of a clip
    int W = 1280, H = 720; sscanf(Arg(argc, argv, "size", "1280x720").c_str(), "%dx%d", &W, &H);
    const int start = atoi(Arg(argc, argv, "start", "0").c_str()), count = atoi(Arg(argc, argv, "count", "60").c_str());
    const int grid = atoi(Arg(argc, argv, "grid", "1").c_str());
    const std::string perfName = Arg(argc, argv, "perf", "slow");
    const bool gray = Arg(argc, argv, "fmt", "abgr") == "gray";   // fmt=gray: the inputs are an 8-bit brightness plane
    const int down = std::max(1, atoi(Arg(argc, argv, "down", "1").c_str()));   // down=2: the whole frame made half the size first (so a HUD at the edges stays in the picture), then cropped to size=
    const int K = std::max(1, atoi(Arg(argc, argv, "scale", "1").c_str()));   // scale=K: the hardware looks at the brightness plane made K times smaller (its search is of limited reach), its vectors are scaled back up (K > 1 needs fmt=gray)
    const int waitMs = atoi(Arg(argc, argv, "wait", "0").c_str());
    if (clipPath.empty() && rawPath.empty()) { printf("flowlab clip=FILE.lsrec start=N count=M out=FOLDER [grid=1|2|4] [perf=slow|medium|fast] [size=WxH]\n"); return 2; }
    CreateDirectoryA(outDir.c_str(), nullptr);

    // ---- the clip: all frames cropped to the window size, the even ones kept as real frames
    std::vector<std::vector<uint8_t>> frames;
    int total = 0;
    if (!rawPath.empty()) {
        FILE* rf = fopen(rawPath.c_str(), "rb");
        if (!rf) { printf("cannot read %s\n", rawPath.c_str()); return 3; }
        _fseeki64(rf, 0, SEEK_END); const long long bytes = _ftelli64(rf); _fseeki64(rf, 0, SEEK_SET);
        const long long one = (long long)W * H * 4;
        total = (int)((bytes / one + 1) & ~1LL);   // real, truth, ..., real: an odd count gets a copy of the last real frame as a last truth
        frames.resize(total);
        for (int i = 0; i < total; ++i) { frames[i].resize((size_t)one); if ((i + 1) * one <= bytes) fread(frames[i].data(), (size_t)one, 1, rf); else frames[i] = frames[i - 1]; }
        fclose(rf);
        FILE* wf = fopen((outDir + "\\frames.rgba").c_str(), "wb"); for (auto& fr : frames) fwrite(fr.data(), fr.size(), 1, wf); fclose(wf);
    } else {
    nr::lsrec::Reader rd; std::string rerr;
    if (!rd.Open(std::wstring(clipPath.begin(), clipPath.end()), &rerr)) { printf("clip: %s\n", rerr.c_str()); return 3; }
    const auto& hd = rd.Header();
    if (hd.bytesPerPixel != 4 || hd.width / down < (uint32_t)W || hd.height / down < (uint32_t)H) { printf("clip: needs an 8-bit clip at least %dx%d\n", W, H); return 3; }
    total = std::min<int>(count, (int)rd.Count() - start) & ~1;
    const uint32_t cx = (hd.width / down - W) / 2, cy = (hd.height / down - H) / 2;
    frames.resize(total);
    {
        std::vector<uint8_t> full;
        FILE* f = fopen((outDir + "\\frames.rgba").c_str(), "wb");
        for (int i = 0; i < total; ++i) {
            if (!rd.Read(start + i, full)) { printf("frame %d unreadable\n", start + i); return 3; }
            frames[i].resize((size_t)W * H * 4);
            if (down == 1) { for (int y = 0; y < H; ++y) memcpy(&frames[i][(size_t)y * W * 4], &full[((size_t)(cy + y) * hd.width + cx) * 4], (size_t)W * 4); }
            else for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) for (int c = 0; c < 4; ++c) {   // the mean of each down x down block
                int sum = 0;
                for (int dy = 0; dy < down; ++dy) for (int dx = 0; dx < down; ++dx) sum += full[((size_t)((cy + y) * down + dy) * hd.width + (cx + x) * down + dx) * 4 + c];
                frames[i][((size_t)y * W + x) * 4 + c] = (uint8_t)(sum / (down * down));
            }
            // (RGBA8 in the file already for the formats a Compact clip has: 28 = R8G8B8A8; 87 = B8G8R8A8 would need a swap)
            if (hd.format == DXGI_FORMAT_B8G8R8A8_UNORM) for (size_t p = 0; p < frames[i].size(); p += 4) std::swap(frames[i][p], frames[i][p + 2]);
            fwrite(frames[i].data(), frames[i].size(), 1, f);
        }
        fclose(f);
    }
    }
    {   // lsrecout=FILE: the frames as a recording (R8G8B8A8, stored raw), for nr_fgeval and the other tools that read .lsrec
        const std::string outRec = Arg(argc, argv, "lsrecout");
        if (!outRec.empty()) {
            nr::lsrec::FileHeader fh; fh.width = W; fh.height = H; fh.format = 28; fh.bytesPerPixel = 4; fh.source = nr::lsrec::kCaptured; fh.content = nr::lsrec::kSdrView; fh.qpcFrequency = 1000000;
            std::vector<nr::lsrec::Frame> fr(total); std::vector<const nr::lsrec::Frame*> ptrs;
            for (int i = 0; i < total; ++i) { fr[i].header.index = i; fr[i].header.qpc = (int64_t)i * 33333; fr[i].header.codec = nr::lsrec::kRaw; fr[i].header.rawBytes = (uint32_t)frames[i].size(); fr[i].data = frames[i]; ptrs.push_back(&fr[i]); }
            std::string err;
            printf("lsrec: %s%s\n", outRec.c_str(), nr::lsrec::Write(std::wstring(outRec.begin(), outRec.end()), fh, ptrs, &err) ? " written" : (" FAILED: " + err).c_str());
        }
    }
    const int reals = total / 2;
    printf("clip: %d frames cropped to %dx%d, %d real frames, %d pairs\n", total, W, H, reals, reals - 1);

    // ---- the NVIDIA adapter
    ComPtr<IDXGIFactory6> factory; CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
        if (d.VendorId == 0x10DE) break;
        adapter.Reset();
    }
    if (!adapter) { printf("no NVIDIA adapter\n"); return 4; }

    // ======================================================== ours: Direct3D 12
    {
        ComPtr<ID3D12Device> dev; CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));
        D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
        ComPtr<ID3D12CommandQueue> queue; CHECK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> alloc; CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
        ComPtr<ID3D12GraphicsCommandList> cl; CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl))); cl->Close();
        ComPtr<ID3D12Fence> fence; CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr); UINT64 fv = 0;
        auto wait = [&]() { queue->Signal(fence.Get(), ++fv); fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, 20000); };

        auto texture = [&](DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& t) {
            D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC rdsc{ D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, (UINT64)W, (UINT)H, 1, 1, fmt, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags };
            return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rdsc, state, nullptr, IID_PPV_ARGS(&t));
        };
        // the real frames on the card (upload one by one)
        std::vector<ComPtr<ID3D12Resource>> real(reals);
        for (int r = 0; r < reals; ++r) {
            CHECK(texture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, real[r]));
            D3D12_RESOURCE_DESC rdsc = real[r]->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows; UINT64 rowBytes, tot;
            dev->GetCopyableFootprints(&rdsc, 0, 1, 0, &fp, &rows, &rowBytes, &tot);
            D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
            D3D12_RESOURCE_DESC bd{ D3D12_RESOURCE_DIMENSION_BUFFER, 0, tot, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
            ComPtr<ID3D12Resource> upload; CHECK(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
            uint8_t* m = nullptr; upload->Map(0, nullptr, (void**)&m);
            for (int y = 0; y < H; ++y) memcpy(m + (size_t)fp.Footprint.RowPitch * y, &frames[2 * r][(size_t)y * W * 4], (size_t)W * 4);
            upload->Unmap(0, nullptr);
            alloc->Reset(); cl->Reset(alloc.Get(), nullptr);
            D3D12_TEXTURE_COPY_LOCATION dst{ real[r].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src{ upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; src.PlacedFootprint = fp;
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = { real[r].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
            cl->ResourceBarrier(1, &b); cl->Close();
            ID3D12CommandList* ls[] = { cl.Get() }; queue->ExecuteCommandLists(1, ls); wait();
        }
        ComPtr<ID3D12Resource> motion, distrust;
        CHECK(texture(DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, motion));
        CHECK(texture(DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, distrust));
        D3D12_RESOURCE_DESC mdesc = motion->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT mfp{}; UINT mrows; UINT64 mrowBytes, mtot;
        dev->GetCopyableFootprints(&mdesc, 0, 1, 0, &mfp, &mrows, &mrowBytes, &mtot);
        D3D12_HEAP_PROPERTIES rp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC rbd{ D3D12_RESOURCE_DIMENSION_BUFFER, 0, mtot, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
        ComPtr<ID3D12Resource> readback; CHECK(dev->CreateCommittedResource(&rp, D3D12_HEAP_FLAG_NONE, &rbd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));

        FlowEstimator est;
        if (!est.Init(dev.Get(), [](const char*) {})) { printf("flow estimator did not start\n"); return 5; }
        est.Ensure(W, H);
        FILE* out = fopen((outDir + "\\ours.f32").c_str(), "wb");
        std::vector<float> flow((size_t)W * H * 2);
        FILE* outRev = nullptr; bool rev = false;
        // one frame through the estimator; with `keep`, the vectors it made (this frame to the one before it) go to the file
        auto frameThrough = [&](int r, int slot, bool keep) {
            alloc->Reset(); cl->Reset(alloc.Get(), nullptr);
            est.Record(cl.Get(), slot % FlowEstimator::kSlots, real[r].Get(), DXGI_FORMAT_R8G8B8A8_UNORM, motion.Get(), distrust.Get());
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = { motion.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
            cl->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION dst{ readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint = mfp;
            D3D12_TEXTURE_COPY_LOCATION src{ motion.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; src.SubresourceIndex = 0;
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter); cl->ResourceBarrier(1, &b);
            cl->Close(); ID3D12CommandList* ls[] = { cl.Get() }; queue->ExecuteCommandLists(1, ls); wait();
            if (!keep) return;
            const uint8_t* m = nullptr; readback->Map(0, nullptr, (void**)&m);
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                const uint16_t* h2 = (const uint16_t*)(m + (size_t)mfp.Footprint.RowPitch * y) + x * 2;
                flow[((size_t)y * W + x) * 2 + 0] = HalfToFloat(h2[0]); flow[((size_t)y * W + x) * 2 + 1] = HalfToFloat(h2[1]);
            }
            readback->Unmap(0, nullptr);
            fwrite(flow.data(), flow.size() * 4, 1, keep == true && rev ? outRev : out);
        };
        for (int r = 0; r < reals; ++r) frameThrough(r, r, r > 0);   // the flow at C pointing back to A (ours.f32)
        // the other way: the later frame first, then the earlier one: the flow at A pointing forward to C (ours_rev.f32)
        outRev = fopen((outDir + "\\ours_rev.f32").c_str(), "wb"); rev = true;
        for (int r = 1; r < reals; ++r) { est.Forget(); frameThrough(r, 0, false); frameThrough(r - 1, 1, true); }
        fclose(outRev);
        fclose(out);
        est.Shutdown();
        printf("ours: %d flow fields written\n", reals - 1);
    }

    // ======================================================== NVIDIA optical flow: Direct3D 11
    {
        ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
        CHECK(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx));
        HMODULE dll = LoadLibraryW(L"nvofapi64.dll");
        if (!dll) { printf("nvofapi64.dll is not on this machine\n"); return 6; }
        auto create = (NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D11_API_FUNCTION_LIST*))GetProcAddress(dll, "NvOFAPICreateInstanceD3D11");
        if (!create) { printf("NvOFAPICreateInstanceD3D11 is not exported\n"); return 6; }
        NV_OF_D3D11_API_FUNCTION_LIST api{};
        OF_CHECK(create(NV_OF_API_VERSION, &api));
        NvOFHandle of = nullptr;
        OF_CHECK(api.nvCreateOpticalFlowD3D11(dev.Get(), ctx.Get(), &of));
        {
            uint32_t sizes = 0, size = sizeof sizes;
            if (api.nvOFGetCaps(of, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, nullptr, &size) == NV_OF_SUCCESS) { std::vector<uint32_t> v(size / 4); api.nvOFGetCaps(of, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, v.data(), &size); printf("ofa: output grid sizes (mask):"); for (auto x : v) printf(" %u", x); printf("\n"); }
            (void)sizes;
        }
        NV_OF_INIT_PARAMS ip{};
        const int OW = W / K, OH = H / K;
        ip.width = OW; ip.height = OH;
        ip.outGridSize = grid == 4 ? NV_OF_OUTPUT_VECTOR_GRID_SIZE_4 : grid == 2 ? NV_OF_OUTPUT_VECTOR_GRID_SIZE_2 : NV_OF_OUTPUT_VECTOR_GRID_SIZE_1;
        ip.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
        ip.mode = NV_OF_MODE_OPTICALFLOW;
        ip.perfLevel = perfName == "fast" ? NV_OF_PERF_LEVEL_FAST : perfName == "medium" ? NV_OF_PERF_LEVEL_MEDIUM : NV_OF_PERF_LEVEL_SLOW;
        ip.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
        ip.inputBufferFormat = gray ? NV_OF_BUFFER_FORMAT_GRAYSCALE8 : NV_OF_BUFFER_FORMAT_ABGR8;
        OF_CHECK(api.nvOFInit(of, &ip));

        // the buffers: inputs are our own textures (registered), the output is a texture of the format the API asks for
        auto texture2d = [&](UINT w, UINT h, DXGI_FORMAT fmt, D3D11_USAGE usage, UINT bind, UINT cpu, const void* data, UINT pitch, ComPtr<ID3D11Texture2D>& t) {
            D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1; d.Usage = usage; d.BindFlags = bind; d.CPUAccessFlags = cpu;
            D3D11_SUBRESOURCE_DATA sd{ data, pitch, 0 };
            return dev->CreateTexture2D(&d, data ? &sd : nullptr, &t);
        };
        std::vector<ComPtr<ID3D11Texture2D>> inTex(reals);
        std::vector<NvOFGPUBufferHandle> inBuf(reals);
        for (int r = 0; r < reals; ++r) {
            if (gray) {
                std::vector<uint8_t> g((size_t)OW * OH);
                for (int y = 0; y < OH; ++y) for (int x = 0; x < OW; ++x) {
                    int sum = 0;
                    for (int dy = 0; dy < K; ++dy) for (int dx = 0; dx < K; ++dx) { const uint8_t* px = &frames[2 * r][((size_t)(y * K + dy) * W + x * K + dx) * 4]; sum += (px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8; }
                    g[(size_t)y * OW + x] = (uint8_t)(sum / (K * K));
                }
                CHECK(texture2d(OW, OH, DXGI_FORMAT_R8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, g.data(), OW, inTex[r]));
            } else
            CHECK(texture2d(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, frames[2 * r].data(), W * 4, inTex[r]));
            OF_CHECK(api.nvOFRegisterResourceD3D11(of, inTex[r].Get(), &inBuf[r]));
        }
        uint32_t fcount = 0; OF_CHECK(api.nvOFGetSurfaceFormatCountD3D11(of, NV_OF_BUFFER_USAGE_OUTPUT, NV_OF_MODE_OPTICALFLOW, &fcount));
        std::vector<DXGI_FORMAT> fmts(fcount); OF_CHECK(api.nvOFGetSurfaceFormatD3D11(of, NV_OF_BUFFER_USAGE_OUTPUT, NV_OF_MODE_OPTICALFLOW, fmts.data()));
        const DXGI_FORMAT outFmt = fmts.empty() ? DXGI_FORMAT_R16G16_SINT : fmts[0];
        const UINT gw = (OW + grid - 1) / grid, gh = (OH + grid - 1) / grid;
        ComPtr<ID3D11Texture2D> outTex, stage;
        CHECK(texture2d(gw, gh, outFmt, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, nullptr, 0, outTex));
        CHECK(texture2d(gw, gh, outFmt, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, nullptr, 0, stage));
        NvOFGPUBufferHandle outBuf = nullptr;
        OF_CHECK(api.nvOFRegisterResourceD3D11(of, outTex.Get(), &outBuf));
        printf("ofa: grid %d, perf %s, output format %u, %ux%u vectors\n", grid, perfName.c_str(), (unsigned)outFmt, gw, gh);

        FILE* out = fopen((outDir + "\\ofa.f32").c_str(), "wb");
        std::vector<float> flow((size_t)W * H * 2);
        double msSum = 0; int msN = 0;
        for (int r = 1; r < reals; ++r) {
            NV_OF_EXECUTE_INPUT_PARAMS in{}; NV_OF_EXECUTE_OUTPUT_PARAMS outp{};
            in.inputFrame = inBuf[r]; in.referenceFrame = inBuf[r - 1]; in.disableTemporalHints = NV_OF_TRUE;   // C to A: each pair stands alone here
            outp.outputBuffer = outBuf;
            const auto t0 = std::chrono::steady_clock::now();
            OF_CHECK(api.nvOFExecute(of, &in, &outp));
            if (waitMs > 0) Sleep(waitMs);   // (the hardware works on its own: does reading too soon give a half-written field?)
            ctx->CopyResource(stage.Get(), outTex.Get());
            D3D11_MAPPED_SUBRESOURCE mp{};
            CHECK(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &mp));   // (waits for the hardware)
            msSum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); ++msN;
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                const int cell = grid * K;   // a vector covers grid x K pixels of the full picture
                const int16_t* v = (const int16_t*)((const uint8_t*)mp.pData + (size_t)mp.RowPitch * std::min<int>(y / cell, gh - 1)) + std::min<int>(x / cell, gw - 1) * 2;
                flow[((size_t)y * W + x) * 2 + 0] = v[0] / 32.0f * K; flow[((size_t)y * W + x) * 2 + 1] = v[1] / 32.0f * K;
            }
            ctx->Unmap(stage.Get(), 0);
            fwrite(flow.data(), flow.size() * 4, 1, out);
        }
        fclose(out);
        printf("ofa: %d flow fields written, %.2f ms a pair (execute and readback)\n", msN, msN ? msSum / msN : 0.0);
        api.nvOFDestroy(of);
    }
    return 0;
}
