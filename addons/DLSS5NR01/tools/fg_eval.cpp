// nr_fgeval: how well a frame generator rebuilds frames of a real recording, offline (no Lossless Scaling, no game).
//
// It reads a .lsrec recording (the recorder's; made with frame generation off, so every frame is a real one), keeps every other frame and
// rebuilds each dropped one from the two kept around it, then scores the rebuilt frame against the real one. Scored beside it, two
// baselines: the frame before shown again ("hold") and the two kept frames mixed half and half ("blend"). The worst frames are written
// as pictures (the real frame | the generator's | the blend) so the failures can be seen.
//
// The generator here is AMD's FSR 3.1 frame generation (the FidelityFX runtime the FSR Upscaler ships), driven through its API without a
// swap chain, and fed this project's motion estimate (src/engine/flow_estimator.cpp) with a flat depth, as the upscalers are.
// Frames are compared in their SDR view: HDR recordings are rolled off as screenshots show them.
//
//   nr_fgeval <recording.lsrec> <output folder> [first=N] [count=N] [fsr=<amd_fidelityfx_dx12.dll>] [worst=N]
#include <windows.h>
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
#include "engine/flow_estimator.h"
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_framegeneration.h"
#include "ffx_api/ffx_api_loader.h"
#include "ffx_api/dx12/ffx_api_dx12.h"

namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

std::wstring Wide(const char* s) { const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0); std::wstring w(n > 0 ? n - 1 : 0, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n); return w; }
int Arg(int argc, char** argv, const char* key, int fallback) {
    const size_t k = strlen(key);
    for (int i = 3; i < argc; ++i) if (!strncmp(argv[i], key, k) && argv[i][k] == '=') return atoi(argv[i] + k + 1);
    return fallback;
}
std::string ArgText(int argc, char** argv, const char* key) {
    const size_t k = strlen(key);
    for (int i = 3; i < argc; ++i) if (!strncmp(argv[i], key, k) && argv[i][k] == '=') return argv[i] + k + 1;
    return {};
}

// A frame of the recording as 8-bit RGBA in its SDR view. Light (the upscalers' HDR frames, 1 = the SDR white) and scRGB are rolled off
// above 0.75 as the screenshots do, then sRGB-encoded; an SDR view is taken as it is; 8-bit frames as they are (BGRA swapped to RGBA).
bool ToRgba8(const nr::lsrec::FileHeader& h, const std::vector<uint8_t>& px, std::vector<uint8_t>& out) {
    const size_t n = static_cast<size_t>(h.width) * h.height;
    out.resize(n * 4);
    if (h.bytesPerPixel == 4) {
        const bool bgra = h.format == DXGI_FORMAT_B8G8R8A8_UNORM || h.format == DXGI_FORMAT_B8G8R8X8_UNORM;
        for (size_t i = 0; i < n; ++i) {
            out[i * 4 + 0] = px[i * 4 + (bgra ? 2 : 0)]; out[i * 4 + 1] = px[i * 4 + 1]; out[i * 4 + 2] = px[i * 4 + (bgra ? 0 : 2)]; out[i * 4 + 3] = 255;
        }
        return true;
    }
    if (h.bytesPerPixel != 8) return false;
    const uint16_t* in = reinterpret_cast<const uint16_t*>(px.data());
    const float scale = h.content == nr::lsrec::kOwnEncoding ? 80.0f / 200.0f : 1.0f;   // scRGB of its own: 1.0 = 80 nits, SDR white taken as 200
    for (size_t i = 0; i < n; ++i) for (int c = 0; c < 3; ++c) {
        float v = DirectX::PackedVector::XMConvertHalfToFloat(in[i * 4 + c]);
        if (h.content != nr::lsrec::kSdrView) {
            v = std::max(v * scale, 0.0f);
            if (v > 0.75f) v = 0.75f + 0.25f * (1.0f - std::exp(-(v - 0.75f) / 0.25f));
            v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        }
        out[i * 4 + c] = static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        if (c == 2) out[i * 4 + 3] = 255;
    }
    return true;
}

// Peak signal-to-noise ratio of two RGBA8 pictures over R, G and B (dB; higher is closer), and the mean absolute difference (levels).
double Psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, double* meanAbs) {
    double se = 0, ae = 0; size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4) for (int c = 0; c < 3; ++c) { const double d = double(a[i + c]) - double(b[i + c]); se += d * d; ae += std::abs(d); ++n; }
    if (meanAbs) *meanAbs = n ? ae / n : 0;
    const double mse = n ? se / n : 0;
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

bool WriteBmp(const std::wstring& path, const std::vector<const std::vector<uint8_t>*>& tiles, uint32_t w, uint32_t h) {
    const uint32_t W = w * static_cast<uint32_t>(tiles.size()), rowBytes = W * 3, pad = (4 - rowBytes % 4) % 4;
    FILE* f = _wfopen(path.c_str(), L"wb"); if (!f) return false;
    const uint32_t imageSize = (rowBytes + pad) * h, fileSize = 54 + imageSize;
    uint8_t hdr[54] = { 'B', 'M' };
    auto put32 = [&](int at, uint32_t v) { memcpy(hdr + at, &v, 4); };
    put32(2, fileSize); put32(10, 54); put32(14, 40); put32(18, W); put32(22, h); hdr[26] = 1; hdr[28] = 24; put32(34, imageSize);
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> row(rowBytes + pad, 0);
    for (int y = static_cast<int>(h) - 1; y >= 0; --y) {
        for (size_t t = 0; t < tiles.size(); ++t) for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = tiles[t]->data() + (static_cast<size_t>(y) * w + x) * 4;
            uint8_t* o = row.data() + (t * w + x) * 3; o[0] = p[2]; o[1] = p[1]; o[2] = p[0];
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    return true;
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

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { printf("usage: nr_fgeval <recording.lsrec> <output folder> [first=N] [count=N] [fsr=<amd_fidelityfx_dx12.dll>] [worst=N]\n"); return 2; }
    nr::lsrec::Reader rec; std::string error;
    if (!rec.Open(Wide(argv[1]), &error)) { printf("%s: %s\n", argv[1], error.c_str()); return 2; }
    const std::wstring outDir = Wide(argv[2]); CreateDirectoryW(outDir.c_str(), nullptr);
    const nr::lsrec::FileHeader& h = rec.Header();
    const uint32_t W = h.width, H = h.height;
    const int first = std::max(0, Arg(argc, argv, "first", 0));
    const int count = std::min<int>(Arg(argc, argv, "count", 1 << 30), static_cast<int>(rec.Count()) - first);
    const int worstShown = Arg(argc, argv, "worst", 3);
    if (count < 3) { printf("the recording has too few frames (%zu)\n", rec.Count()); return 2; }

    // the frames, in their SDR view
    std::vector<std::vector<uint8_t>> frames(count); std::vector<double> times(count);
    { std::vector<uint8_t> px;
      for (int i = 0; i < count; ++i) {
          if (!rec.Read(first + i, px) || !ToRgba8(h, px, frames[i])) { printf("frame %d could not be read or converted (format %u)\n", first + i, h.format); return 3; }
          times[i] = h.qpcFrequency ? rec.FrameInfo(first + i).qpc * 1000.0 / static_cast<double>(h.qpcFrequency) : i * 16.7;   // ms
      } }
    printf("%ls: %ux%u, %d frames from %d; every other one is rebuilt\n", Wide(argv[1]).c_str(), W, H, count, first);

    Gpu g; if (!g.Init()) { printf("no Direct3D 12 device\n"); return 4; }
    // AMD's runtime, as the FSR Upscaler ships it (next to this tool's build: fsr\amd_fidelityfx_dx12.dll)
    std::wstring dll = Wide(ArgText(argc, argv, "fsr").c_str());
    if (dll.empty()) { wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH); dll = exe; dll = dll.substr(0, dll.find_last_of(L'\\')) + L"\\fsr\\amd_fidelityfx_dx12.dll"; }
    HMODULE ffxModule = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!ffxModule) { printf("AMD's runtime could not be loaded from %ls\n", dll.c_str()); return 4; }
    ffxFunctions fx{}; ffxLoadFunctions(&fx, ffxModule);
    if (!fx.CreateContext || !fx.Configure || !fx.Dispatch) { printf("AMD's runtime lacks the FidelityFX API\n"); return 4; }

    // FSR's frame generation runs at present time, on a swap chain of FidelityFX's own (as a game with FSR 3 has it, and as OptiScaler sets
    // it up): here on a window that is never shown. The generated frame is taken in the generation callback, as it is made.
    WNDCLASSEXW wc{ sizeof wc }; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"NrFgEval"; RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"nr fgeval", WS_POPUP, 0, 0, 320, 180, nullptr, nullptr, wc.hInstance, nullptr);
    IDXGIFactory4* factory = nullptr; CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = W; sd.Height = H; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 3; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain4* chain = nullptr; ffxContext chainCtx = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 scd{}; scd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    scd.swapchain = &chain; scd.hwnd = hwnd; scd.desc = &sd; scd.fullscreenDesc = nullptr; scd.dxgiFactory = factory; scd.gameQueue = g.queue;
    if (const ffxReturnCode_t rc = fx.CreateContext(&chainCtx, &scd.header, nullptr); rc != FFX_API_RETURN_OK || !chain) { printf("FidelityFX's swap chain could not be made (code %u)\n", rc); return 4; }
    ffxCreateBackendDX12Desc backend{}; backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; backend.device = g.dev;
    ffxCreateContextDescFrameGeneration create{}; create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION; create.header.pNext = &backend.header;
    create.flags = 0; create.displaySize = { W, H }; create.maxRenderSize = { W, H }; create.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    ffxContext ctx = nullptr;
    if (fx.CreateContext(&ctx, &create.header, nullptr) != FFX_API_RETURN_OK || !ctx) { printf("FSR frame generation could not make its context\n"); return 4; }

    FlowEstimator est;
    if (!est.Init(g.dev, [](const char* m) { printf("  %s\n", m); }) || !est.Ensure(W, H)) { printf("the motion estimate could not start\n"); return 4; }

    // textures: the kept frame (read by the estimate and by FSR), the motion vectors and the distrust mask, a flat depth, the generated frame
    ID3D12Resource* cur = g.Texture(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* motion = g.Texture(W, H, DXGI_FORMAT_R16G16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* distrust = g.Texture(W, H, DXGI_FORMAT_R8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* depth = g.Texture(W, H, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* gen = g.Texture(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC texDesc = cur->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    g.dev->GetCopyableFootprints(&texDesc, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    ID3D12Resource* upload = g.Buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* readback = g.Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!cur || !motion || !distrust || !depth || !gen || !upload || !readback) { printf("textures could not be made\n"); return 4; }
    {   // the flat depth (Lossless Scaling has none), once
        D3D12_RESOURCE_DESC dd = depth->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT dfp{}; UINT drows = 0; UINT64 drow = 0, dtotal = 0;
        g.dev->GetCopyableFootprints(&dd, 0, 1, 0, &dfp, &drows, &drow, &dtotal);
        ID3D12Resource* dup = g.Buffer(dtotal, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* m = nullptr; dup->Map(0, nullptr, reinterpret_cast<void**>(&m));
        const std::vector<float> row(W, 0.5f);
        for (UINT y = 0; y < drows; ++y) memcpy(m + dfp.Offset + y * dfp.Footprint.RowPitch, row.data(), W * 4);
        dup->Unmap(0, nullptr);
        g.Begin();
        D3D12_TEXTURE_COPY_LOCATION to{ depth, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; to.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION from{ dup, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; from.PlacedFootprint = dfp;
        g.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        g.Barrier(depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g.Submit(); dup->Release();
    }

    static ffxFunctions* s_fx = &fx;
    static ID3D12Resource* s_readback = readback;
    static D3D12_PLACED_SUBRESOURCE_FOOTPRINT s_fp = fp;
    static volatile LONG s_generated = 0;
    ffxConfigureDescFrameGeneration cfg{}; cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = chain; cfg.frameGenerationEnabled = true; cfg.allowAsyncWorkloads = false; cfg.onlyPresentGenerated = false;
    cfg.generationRect = { 0, 0, static_cast<int>(W), static_cast<int>(H) };
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
    if (const ffxReturnCode_t rc = fx.Configure(&ctx, &cfg.header); rc != FFX_API_RETURN_OK) { printf("FSR frame generation could not be configured (code %u)\n", rc); return 4; }

    struct Score { int frame; double fsr, hold, blend, fsrAbs; std::vector<uint8_t> picture, mix; };
    std::vector<Score> scores;
    D3D12_RESOURCE_STATES curState = D3D12_RESOURCE_STATE_COPY_DEST;
    uint64_t frameId = 0;
    for (int i = 0; i < count; i += 2) {   // the kept frames: 0, 2, 4...; each one's generated frame stands for the one before it (i - 1)
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
        // the motion from the kept frame before to this one (the estimate keeps the frame before itself)
        est.Record(g.list, 0, cur, DXGI_FORMAT_R8G8B8A8_UNORM, motion, distrust);
        g.Barrier(motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // FSR: this frame's depth and motion, then a frame between the one before and this one
        ffxDispatchDescFrameGenerationPrepare prep{}; prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        prep.frameID = frameId; prep.commandList = g.list; prep.renderSize = { W, H }; prep.jitterOffset = { 0, 0 }; prep.motionVectorScale = { 1.0f, 1.0f };
        prep.frameTimeDelta = i >= 2 ? static_cast<float>(times[i] - times[i - 2]) : 16.7f;
        prep.cameraNear = 0.1f; prep.cameraFar = 1000.0f; prep.cameraFovAngleVertical = 1.0f; prep.viewSpaceToMetersFactor = 1.0f;
        prep.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        prep.motionVectors = ffxApiGetResourceDX12(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        const ffxReturnCode_t rp = fx.Dispatch(&ctx, &prep.header);
        if (rp != FFX_API_RETURN_OK) { printf("frame %d: FSR's prepare failed (code %u)\n", first + i, rp); return 5; }
        g.Barrier(motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12Resource* back = nullptr; chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back));
        g.Barrier(cur, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g.Barrier(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        g.list->CopyResource(back, cur);
        g.Barrier(back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        g.Barrier(cur, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g.Submit();
        back->Release();
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
        cfg.frameID = frameId;
        fx.Configure(&ctx, &cfg.header);
        const LONG before = s_generated;
        if (FAILED(chain->Present(0, 0))) { printf("frame %d: present failed\n", first + i); return 5; }
        if (i >= 2) {   // wait for the generated frame (the callback, then the copy on the GPU)
            const ULONGLONG t0 = GetTickCount64();
            while ((s_generated == before || marked(false)) && GetTickCount64() - t0 < 3000) Sleep(1);
            if (s_generated == before || marked(false)) { printf("frame %d: no generated frame came within 3 s\n", first + i); return 5; }
        } else Sleep(50);
        ++frameId;
        if (i < 2) continue;   // the first kept frame has no frame before it
        Score s; s.frame = first + i - 1; s.picture.resize(static_cast<size_t>(W) * H * 4);
        { uint8_t* m = nullptr; readback->Map(0, nullptr, reinterpret_cast<void**>(&m));
          for (uint32_t y = 0; y < H; ++y) memcpy(s.picture.data() + static_cast<size_t>(y) * W * 4, m + fp.Offset + y * fp.Footprint.RowPitch, W * 4);
          D3D12_RANGE none{ 0, 0 }; readback->Unmap(0, &none); }
        const std::vector<uint8_t>& truth = frames[i - 1];
        s.mix.resize(truth.size());
        for (size_t k = 0; k < truth.size(); ++k) s.mix[k] = static_cast<uint8_t>((frames[i - 2][k] + frames[i][k] + 1) / 2);
        s.fsr = Psnr(s.picture, truth, &s.fsrAbs); s.hold = Psnr(frames[i - 2], truth, nullptr); s.blend = Psnr(s.mix, truth, nullptr);
        printf("  frame %4d  FSR %5.2f dB (%.2f levels off)   hold %5.2f dB   blend %5.2f dB\n", s.frame, s.fsr, s.fsrAbs, s.hold, s.blend);
        scores.push_back(std::move(s));
    }
    if (scores.empty()) { printf("nothing was rebuilt\n"); return 5; }
    double fsr = 0, hold = 0, blend = 0; int better = 0;
    for (const Score& s : scores) { fsr += s.fsr; hold += s.hold; blend += s.blend; better += s.fsr > std::max(s.hold, s.blend) ? 1 : 0; }
    const double n = static_cast<double>(scores.size());
    printf("average over %zu rebuilt frames: FSR %.2f dB, hold %.2f dB, blend %.2f dB; FSR the closest in %d of them\n", scores.size(), fsr / n, hold / n, blend / n, better);
    std::vector<const Score*> order; for (const Score& s : scores) order.push_back(&s);
    std::sort(order.begin(), order.end(), [](const Score* a, const Score* b) { return a->fsr < b->fsr; });
    for (int k = 0; k < worstShown && k < static_cast<int>(order.size()); ++k) {
        const Score& s = *order[k];
        wchar_t name[64]; swprintf(name, 64, L"\\worst%d_frame%05d.bmp", k + 1, s.frame);
        WriteBmp(outDir + name, { &frames[s.frame - first], &s.picture, &s.mix }, W, H);
        printf("  worst %d: frame %d, FSR %.2f dB: %ls (the real frame | FSR | the blend)\n", k + 1, s.frame, s.fsr, (outDir + name).c_str());
    }
    fx.DestroyContext(&ctx, nullptr);
    return 0;
}
