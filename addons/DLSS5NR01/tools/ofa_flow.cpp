// nr_ofaflow: the motion between frames of a recording from the video card's hardware optical flow engine (NVIDIA Optical Flow SDK, D3D12 API; the engine is in the driver on RTX 20 and
// newer), written in the file format nr_fgeval's mvfile= reads, so that the frame generator can be scored with this motion in place of its own estimate.
//
//   nr_ofaflow <recording.lsrec> <output.mv> [first=N] [count=N] [step=N] [grid=1|2|4] [perf=slow|medium|fast] [sign=1|-1] [time=1]
//
// Frames are kept as nr_fgeval keeps them (first, count, every step-th); for the pair (kept frame 2p, kept frame 2p+2) one W x H block of float32 (x, y) per pixel is written: the flow at
// the later frame's pixels back to the earlier one, in pixels. sign=-1 flips the direction if a check with nr_fgeval mvcheck=1 says the vectors point the other way.
// time=1 times the engine for each pair (milliseconds, the engine alone and with the copies).
// A lab tool: it builds only where the SDK's headers are (CMake NR_OFSDK_DIR, the NvOFInterface folder of the downloaded SDK), and nothing of the SDK is in the repository.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "eval_common.h"
#include "nvOpticalFlowD3D12.h"
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;
using namespace nr::eval;

namespace {

bool Ok(NV_OF_STATUS s, const char* what) { if (s != NV_OF_SUCCESS) { printf("%s failed (NV_OF_STATUS %d)\n", what, static_cast<int>(s)); return false; } return true; }

}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { printf("usage: nr_ofaflow <recording.lsrec> <output.mv> [first=N] [count=N] [step=N] [grid=1|2|4] [perf=slow|medium|fast] [sign=1|-1] [time=1]\n"); return 1; }
    nr::lsrec::Reader rec; std::string error;
    if (!rec.Open(Wide(argv[1]), &error)) { printf("%s: %s\n", argv[1], error.c_str()); return 2; }
    const nr::lsrec::FileHeader& h = rec.Header();
    const uint32_t W = h.width, H = h.height;
    const int first = std::max(0, Arg(argc, argv, "first", 0)), step = std::max(1, Arg(argc, argv, "step", 1));
    int count = std::min<int>(Arg(argc, argv, "count", 1 << 30), (static_cast<int>(rec.Count()) - first + step - 1) / step);
    const uint32_t gridWanted = static_cast<uint32_t>(Arg(argc, argv, "grid", 1));
    const std::string perfName = ArgText(argc, argv, "perf");
    const float sign = static_cast<float>(Arg(argc, argv, "sign", 1)) < 0 ? -1.0f : 1.0f;
    const bool timing = Arg(argc, argv, "time", 0) != 0;
    if (count < 3) { printf("too few frames\n"); return 2; }

    // the frames, as luma
    std::vector<std::vector<uint8_t>> luma(count);
    { std::vector<uint8_t> px, rgba;
      for (int k = 0; k < count; ++k) {
          if (!rec.Read(first + k * step, px) || !ToRgba8(h, px, rgba)) { printf("frame %d could not be read\n", first + k * step); return 3; }
          luma[k].resize(static_cast<size_t>(W) * H);
          for (size_t i = 0; i < luma[k].size(); ++i) luma[k][i] = static_cast<uint8_t>(0.2126f * rgba[i * 4] + 0.7152f * rgba[i * 4 + 1] + 0.0722f * rgba[i * 4 + 2] + 0.5f);
      } }

    // the card and its engine
    ComPtr<IDXGIFactory6> factory; CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter; factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) { printf("no Direct3D 12 device\n"); return 4; }
    DXGI_ADAPTER_DESC1 ad{}; adapter->GetDesc1(&ad); printf("card: %ls\n", ad.Description);
    HMODULE lib = LoadLibraryW(L"nvofapi64.dll");
    if (!lib) { printf("nvofapi64.dll (the driver's optical flow engine) could not be loaded: this card or driver has none\n"); return 4; }
    auto create = reinterpret_cast<NV_OF_STATUS (NVOFAPI*)(uint32_t, NV_OF_D3D12_API_FUNCTION_LIST*)>(GetProcAddress(lib, "NvOFAPICreateInstanceD3D12"));
    if (!create) { printf("NvOFAPICreateInstanceD3D12 not found\n"); return 4; }
    NV_OF_D3D12_API_FUNCTION_LIST api{};
    if (!Ok(create(NV_OF_API_VERSION, &api), "NvOFAPICreateInstanceD3D12")) return 4;
    NvOFHandle of = nullptr;
    if (!Ok(api.nvCreateOpticalFlowD3D12(dev.Get(), &of), "nvCreateOpticalFlowD3D12")) return 4;

    // what the engine takes
    uint32_t grids[8] = {}, nGrids = 0;
    api.nvOFGetCaps(of, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, nullptr, &nGrids);
    if (nGrids > 8) nGrids = 8;
    api.nvOFGetCaps(of, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, grids, &nGrids);
    printf("output grid sizes:"); for (uint32_t i = 0; i < nGrids; ++i) printf(" %u", grids[i]); printf("\n");
    uint32_t grid = gridWanted; { bool fits = false; for (uint32_t i = 0; i < nGrids; ++i) fits |= grids[i] == grid; if (!fits && nGrids) grid = grids[nGrids - 1]; }
    DXGI_FORMAT inFmt = DXGI_FORMAT_UNKNOWN, outFmt = DXGI_FORMAT_UNKNOWN;
    for (NV_OF_BUFFER_USAGE usage : { NV_OF_BUFFER_USAGE_INPUT, NV_OF_BUFFER_USAGE_OUTPUT }) {
        uint32_t n = 0; api.nvOFGetSurfaceFormatCountD3D12(of, usage, NV_OF_MODE_OPTICALFLOW, &n);
        std::vector<DXGI_FORMAT> f(n); api.nvOFGetSurfaceFormatD3D12(of, usage, NV_OF_MODE_OPTICALFLOW, f.data());
        printf("%s formats:", usage == NV_OF_BUFFER_USAGE_INPUT ? "input" : "output"); for (DXGI_FORMAT x : f) printf(" %d", static_cast<int>(x)); printf("\n");
        for (DXGI_FORMAT x : f) { if (usage == NV_OF_BUFFER_USAGE_INPUT && x == DXGI_FORMAT_R8_UNORM) inFmt = x; if (usage == NV_OF_BUFFER_USAGE_OUTPUT && outFmt == DXGI_FORMAT_UNKNOWN) outFmt = x; }
    }
    if (inFmt == DXGI_FORMAT_UNKNOWN || outFmt == DXGI_FORMAT_UNKNOWN) { printf("no usable buffer formats\n"); return 4; }

    NV_OF_INIT_PARAMS init{};
    init.width = W; init.height = H; init.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(grid); init.mode = NV_OF_MODE_OPTICALFLOW;
    init.perfLevel = perfName == "fast" ? NV_OF_PERF_LEVEL_FAST : perfName == "medium" ? NV_OF_PERF_LEVEL_MEDIUM : NV_OF_PERF_LEVEL_SLOW;
    init.predDirection = NV_OF_PRED_DIRECTION_FORWARD; init.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    if (!Ok(api.nvOFInit(of, &init), "nvOFInit")) return 4;
    printf("optical flow: %ux%u, grid %u, perf %s\n", W, H, grid, perfName.empty() ? "slow" : perfName.c_str());

    // queue, the textures, and the fences the engine synchronizes with
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    ComPtr<ID3D12CommandAllocator> alloc; dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ComPtr<ID3D12GraphicsCommandList> list; dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)); list->Close();
    const uint32_t ow = (W + grid - 1) / grid, oh = (H + grid - 1) / grid;
    auto texture = [&](uint32_t w, uint32_t hh, DXGI_FORMAT f) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = hh; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1; d.Format = f;
        ComPtr<ID3D12Resource> r; dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)); return r;
    };
    ComPtr<ID3D12Resource> texIn[2] = { texture(W, H, inFmt), texture(W, H, inFmt) }, texOut = texture(ow, oh, outFmt);
    ComPtr<ID3D12Fence> fenceIn[2], fenceOut; dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fenceIn[0])); dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fenceIn[1]));
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fenceOut));
    NvOFGPUBufferHandle hIn[2] = {}, hOut = nullptr;
    for (int i = 0; i < 2; ++i) {
        NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 r{}; r.resource = texIn[i].Get(); r.hOFGpuBuffer = &hIn[i];
        r.inputFencePoint = { fenceIn[i].Get(), 0 }; r.outputFencePoint = { fenceIn[i].Get(), 1 };
        if (!Ok(api.nvOFRegisterResourceD3D12(of, &r), "register input")) return 4;
    }
    { NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 r{}; r.resource = texOut.Get(); r.hOFGpuBuffer = &hOut; r.inputFencePoint = { fenceOut.Get(), 0 }; r.outputFencePoint = { fenceOut.Get(), 1 };
      if (!Ok(api.nvOFRegisterResourceD3D12(of, &r), "register output")) return 4; }

    // upload / readback buffers
    D3D12_RESOURCE_DESC rdIn = texIn[0]->GetDesc(), rdOut = texOut->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fpIn{}, fpOut{}; UINT64 totalIn = 0, totalOut = 0, rowSize = 0; UINT rows = 0;
    dev->GetCopyableFootprints(&rdIn, 0, 1, 0, &fpIn, &rows, &rowSize, &totalIn);
    dev->GetCopyableFootprints(&rdOut, 0, 1, 0, &fpOut, &rows, &rowSize, &totalOut);
    auto buffer = [&](UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r; dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)); return r;
    };
    ComPtr<ID3D12Resource> upload = buffer(totalIn, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ), readback = buffer(totalOut, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12Fence> idle; dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idle)); uint64_t idleValue = 0; HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto wait = [&]() { queue->Signal(idle.Get(), ++idleValue); idle->SetEventOnCompletion(idleValue, ev); WaitForSingleObject(ev, INFINITE); };
    auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER x{}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource = r; x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; x.Transition.StateBefore = a; x.Transition.StateAfter = b;
        list->ResourceBarrier(1, &x);
    };
    auto put = [&](int slot, const std::vector<uint8_t>& l) {   // luma into texIn[slot]
        uint8_t* m = nullptr; upload->Map(0, nullptr, reinterpret_cast<void**>(&m));
        for (uint32_t y = 0; y < H; ++y) memcpy(m + fpIn.Offset + static_cast<size_t>(y) * fpIn.Footprint.RowPitch, l.data() + static_cast<size_t>(y) * W, W);
        upload->Unmap(0, nullptr);
        alloc->Reset(); list->Reset(alloc.Get(), nullptr);
        barrier(texIn[slot].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{ texIn[slot].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{ upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; src.PlacedFootprint = fpIn;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(texIn[slot].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        list->Close(); ID3D12CommandList* l1[] = { list.Get() }; queue->ExecuteCommandLists(1, l1); wait();
    };

    FILE* out = fopen(argv[2], "wb");
    if (!out) { printf("cannot write %s\n", argv[2]); return 3; }
    uint64_t fenceOutValue = 1;   // (registering the output buffer signalled the fence to 1)
    int pairs = 0; double engineMs = 0, totalMs = 0; double sumLen = 0; uint64_t nVec = 0;
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    std::vector<float> full(static_cast<size_t>(W) * H * 2);
    for (int p = 0; 2 * p + 2 < count; ++p) {
        LARGE_INTEGER t0, t1, t2; QueryPerformanceCounter(&t0);
        put(0, luma[2 * p]); put(1, luma[2 * p + 2]);   // reference = the earlier frame, input = the later one
        QueryPerformanceCounter(&t1);
        NV_OF_FENCE_POINT waitPoints[2] = { { fenceIn[0].Get(), 0 }, { fenceIn[1].Get(), 0 } };
        NV_OF_FENCE_POINT done = { fenceOut.Get(), ++fenceOutValue };
        NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{}; in.inputFrame = hIn[1]; in.referenceFrame = hIn[0]; in.disableTemporalHints = NV_OF_TRUE; in.numFencePoints = 2; in.fencePoint = waitPoints;
        NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 outp{}; outp.outputBuffer = hOut; outp.fencePoint = &done;
        if (!Ok(api.nvOFExecuteD3D12(of, &in, &outp), "nvOFExecuteD3D12")) return 5;
        fenceOut->SetEventOnCompletion(fenceOutValue, ev); WaitForSingleObject(ev, INFINITE);
        QueryPerformanceCounter(&t2);
        engineMs += (t2.QuadPart - t1.QuadPart) * 1000.0 / qf.QuadPart;
        alloc->Reset(); list->Reset(alloc.Get(), nullptr);
        barrier(texOut.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{ readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint = fpOut;
        D3D12_TEXTURE_COPY_LOCATION src{ texOut.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(texOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        list->Close(); ID3D12CommandList* l2[] = { list.Get() }; queue->ExecuteCommandLists(1, l2); wait();
        const uint8_t* m = nullptr; readback->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint8_t**>(&m)));
        for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {
            const int16_t* v = reinterpret_cast<const int16_t*>(m + fpOut.Offset + static_cast<size_t>(std::min(y / grid, oh - 1)) * fpOut.Footprint.RowPitch) + 2 * std::min(x / grid, ow - 1);
            const float fx = sign * v[0] / 32.0f, fy = sign * v[1] / 32.0f;
            full[(static_cast<size_t>(y) * W + x) * 2] = fx; full[(static_cast<size_t>(y) * W + x) * 2 + 1] = fy;
            sumLen += std::sqrt(fx * fx + fy * fy); ++nVec;
        }
        D3D12_RANGE none{ 0, 0 }; readback->Unmap(0, &none);
        fwrite(full.data(), 4, full.size(), out);
        QueryPerformanceCounter(&t2); totalMs += (t2.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart;
        ++pairs;
        if (timing) printf("  pair %d: engine %.2f ms (with the waits), mean flow length so far %.2f px\n", p, (t2.QuadPart - t1.QuadPart) * 1000.0 / qf.QuadPart, nVec ? sumLen / nVec : 0.0);
    }
    fclose(out);
    printf("%d pairs written to %s; engine %.2f ms a pair on average (upload and readback excluded), mean flow length %.2f px\n", pairs, argv[2], pairs ? engineMs / pairs : 0.0, nVec ? sumLen / nVec : 0.0);
    return 0;
}
