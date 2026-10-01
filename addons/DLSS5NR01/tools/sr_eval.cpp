// nr_sreval: an upscaler scored offline on a recording (no Lossless Scaling, no game): each frame is shrunk, upscaled back to its own size
// by the addon's own engine (SrEngine: the motion estimate, then AMD's FSR 3.1, NVIDIA's DLSS or Intel's XeSS, as live), and compared with
// the frame itself, beside the shrunk frame stretched back plainly (bilinear). Temporal upscalers keep a history; where the motion they are
// given is wrong (a character a turning camera follows, background just uncovered) that history trails: the pictures show it.
//
//   nr_sreval <recording.lsrec> <output folder> [first=N] [count=N] [shrink=150] [backend=fsr|dlss|xess] [show=N]
//             [straycap=N] [fast=N] [mask=0] [mvscale=N] [motion=none] [meanweight=100] [gradweight=0] [sample=point] [sharpen=N] [stability=N] [fastp=N]
//
// shrink: the ratio in hundredths (150: 1440p from 960p, as 4K from 1440p). show=N: the N worst frames kept as pictures (the frame | the
// upscaler's | plainly stretched). The checks: straycap=N, the motion estimate's cap on straying from its coarser guess (0: none); fast=N, the
// motion (pixels a frame) from which the upscaler leans on the frame (0: never); mask=0, no distrust mask; mvscale=N, the vectors scaled by
// N/100 as the upscaler is told them; motion=none, no vectors at all.
//
// Silent Hill f, 2026-09-27 (docs/frame-generation-research.md): in a fast turn every upscaler trailed behind a plain stretch at a quarter of
// the size (FSR 39.4, DLSS 36.8, XeSS 39.1 dB against 43.3); fast motion leaning on the frame took FSR to 41.8.
#include <windows.h>
#include <shlobj.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "nvsdk_ngx.h"
#include "engine/ngx_paths.h"
#include "addon/lsrec.h"
#include "engine/sr_engine.h"
#include "addon/vram.h"
#include "eval_common.h"
#include "oracle_flow.h"

using namespace nr::eval;

namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// src (sw x sh RGBA8) resampled to dw x dh by area (each output pixel the average of the source it covers): a fair "rendered smaller"
void ResizeArea(const std::vector<uint8_t>& src, uint32_t sw, uint32_t sh, std::vector<uint8_t>& dst, uint32_t dw, uint32_t dh) {
    dst.assign(static_cast<size_t>(dw) * dh * 4, 255);
    const double rx = double(sw) / dw, ry = double(sh) / dh;
    for (uint32_t y = 0; y < dh; ++y) for (uint32_t x = 0; x < dw; ++x) {
        const double x0 = x * rx, x1 = (x + 1) * rx, y0 = y * ry, y1 = (y + 1) * ry;
        double acc[3] = {}, wsum = 0;
        for (uint32_t sy = static_cast<uint32_t>(y0); sy < std::min<double>(sh, std::ceil(y1)); ++sy) {
            const double wy = std::min<double>(sy + 1, y1) - std::max<double>(sy, y0);
            for (uint32_t sx = static_cast<uint32_t>(x0); sx < std::min<double>(sw, std::ceil(x1)); ++sx) {
                const double w = wy * (std::min<double>(sx + 1, x1) - std::max<double>(sx, x0));
                const uint8_t* p = src.data() + (static_cast<size_t>(sy) * sw + sx) * 4;
                for (int c = 0; c < 3; ++c) acc[c] += w * p[c];
                wsum += w;
            }
        }
        for (int c = 0; c < 3; ++c) dst[(static_cast<size_t>(y) * dw + x) * 4 + c] = static_cast<uint8_t>(acc[c] / wsum + 0.5);
    }
}

// src (sw x sh) stretched to dw x dh, bilinear (pixel centres aligned)
void ResizeBilinear(const std::vector<uint8_t>& src, uint32_t sw, uint32_t sh, std::vector<uint8_t>& dst, uint32_t dw, uint32_t dh) {
    dst.assign(static_cast<size_t>(dw) * dh * 4, 255);
    for (uint32_t y = 0; y < dh; ++y) for (uint32_t x = 0; x < dw; ++x) {
        const float fx = std::clamp((x + 0.5f) * sw / dw - 0.5f, 0.0f, sw - 1.0f), fy = std::clamp((y + 0.5f) * sh / dh - 0.5f, 0.0f, sh - 1.0f);
        const uint32_t x0 = static_cast<uint32_t>(fx), y0 = static_cast<uint32_t>(fy), x1 = std::min(x0 + 1, sw - 1), y1 = std::min(y0 + 1, sh - 1);
        const float ax = fx - x0, ay = fy - y0;
        for (int c = 0; c < 3; ++c) {
            auto at = [&](uint32_t xx, uint32_t yy) { return float(src[(static_cast<size_t>(yy) * sw + xx) * 4 + c]); };
            dst[(static_cast<size_t>(y) * dw + x) * 4 + c] = static_cast<uint8_t>((at(x0, y0) * (1 - ax) + at(x1, y0) * ax) * (1 - ay) + (at(x0, y1) * (1 - ax) + at(x1, y1) * ax) * ay + 0.5f);
        }
    }
}

// src (sw x sh) stretched to dw x dh with Catmull-Rom (the picture the upscalers lean on, on the CPU): realonly=1 gives it to the frames the upscaler is not run on
float CatmullRom(float t, float p0, float p1, float p2, float p3) { return 0.5f * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t); }
void ResizeCubic(const std::vector<uint8_t>& src, uint32_t sw, uint32_t sh, std::vector<uint8_t>& dst, uint32_t dw, uint32_t dh) {
    std::vector<float> mid(static_cast<size_t>(dw) * sh * 3);   // across first, then down
    for (uint32_t y = 0; y < sh; ++y) for (uint32_t x = 0; x < dw; ++x) {
        const float fx = (x + 0.5f) * sw / dw - 0.5f; const int ix = static_cast<int>(std::floor(fx)); const float a = fx - ix;
        for (int c = 0; c < 3; ++c) {
            auto at = [&](int xx) { return float(src[(static_cast<size_t>(y) * sw + std::clamp(xx, 0, static_cast<int>(sw) - 1)) * 4 + c]); };
            mid[(static_cast<size_t>(y) * dw + x) * 3 + c] = CatmullRom(a, at(ix - 1), at(ix), at(ix + 1), at(ix + 2));
        }
    }
    dst.assign(static_cast<size_t>(dw) * dh * 4, 255);
    for (uint32_t y = 0; y < dh; ++y) {
        const float fy = (y + 0.5f) * sh / dh - 0.5f; const int iy = static_cast<int>(std::floor(fy)); const float a = fy - iy;
        for (uint32_t x = 0; x < dw; ++x) for (int c = 0; c < 3; ++c) {
            auto at = [&](int yy) { return mid[(static_cast<size_t>(std::clamp(yy, 0, static_cast<int>(sh) - 1)) * dw + x) * 3 + c]; };
            dst[(static_cast<size_t>(y) * dw + x) * 4 + c] = static_cast<uint8_t>(std::clamp(CatmullRom(a, at(iy - 1), at(iy), at(iy + 1), at(iy + 2)) + 0.5f, 0.0f, 255.0f));
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    // debug=1: the D3D12 debug layer (debug=2: with GPU-based validation, slow) and every message it has for the engine's device printed; needs the Graphics Tools feature of Windows
    const int debugLayer = Arg(argc, argv, "debug", 0);
    if (debugLayer) {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            ID3D12Debug1* dbg1 = nullptr;
            if (debugLayer > 1 && SUCCEEDED(dbg->QueryInterface(IID_PPV_ARGS(&dbg1)))) { dbg1->SetEnableGPUBasedValidation(TRUE); dbg1->Release(); }
            dbg->Release();
        } else printf("the D3D12 debug layer is not installed\n");
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { printf("usage: nr_sreval <recording.lsrec> <output folder> [first=N] [count=N] [shrink=150] [backend=fsr|dlss|xess] [show=N] [straycap=N] [fast=N] [mask=0] [mvscale=N] [motion=none] [meanweight=100] [gradweight=0] [sample=point] [sharpen=N] [stability=N] [fastp=N]\n"); return 2; }
    nr::lsrec::Reader rec; std::string error;
    if (!rec.Open(Wide(argv[1]), &error)) { printf("%s: %s\n", argv[1], error.c_str()); return 2; }
    std::wstring outDir = Wide(argv[2]);
    { wchar_t full[MAX_PATH]; if (GetFullPathNameW(outDir.c_str(), MAX_PATH, full, nullptr)) outDir = full; SHCreateDirectoryExW(nullptr, outDir.c_str(), nullptr); }
    const nr::lsrec::FileHeader& h = rec.Header();
    // splice=<other recording> spliceat=N: from frame N of this run on, the frames come from the other recording (a hard cut between two scenes, to see what the upscaler does with its history)
    nr::lsrec::Reader rec2; const std::string splicePath = ArgText(argc, argv, "splice"); const int spliceAt = Arg(argc, argv, "spliceat", 1 << 30);
    if (!splicePath.empty() && (!rec2.Open(Wide(splicePath.c_str()), &error) || rec2.Header().width != h.width || rec2.Header().height != h.height)) { printf("the recording to splice in could not be opened or has another size\n"); return 2; }
    const uint32_t W = h.width, H = h.height;
    const int first = std::max(0, Arg(argc, argv, "first", 0));
    const int count = std::min<int>(Arg(argc, argv, "count", 1 << 30), static_cast<int>(rec.Count()) - first);
    const double shrink = std::max(1.0, Arg(argc, argv, "shrink", 150) / 100.0);
    const uint32_t w = static_cast<uint32_t>(W / shrink + 0.5) & ~1u, hh = static_cast<uint32_t>(H / shrink + 0.5) & ~1u;
    const std::string backendName = ArgText(argc, argv, "backend").empty() ? "fsr" : ArgText(argc, argv, "backend");
    const SrEngine::Backend backend = backendName == "dlss" ? SrEngine::Backend::Dlss : backendName == "xess" ? SrEngine::Backend::Xess : SrEngine::Backend::Fsr;
    const int show = Arg(argc, argv, "show", 3);
    const bool pointSampled = ArgText(argc, argv, "sample") == "point";
    // hdr=1: an HDR recording (fp16 scRGB frames) kept as fp16 light all the way through the engine, as it is live (SDR white 200 nits, as ToRgba8 takes it), and toned down to 8 bits only to be scored
    const bool hdrMode = Arg(argc, argv, "hdr", 0) != 0 && h.bytesPerPixel == 8;
    const DXGI_FORMAT frameFmt = hdrMode ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    const float hdrScale = h.content == nr::lsrec::kOwnEncoding ? 80.0f / 200.0f : 1.0f;
    const bool noMotion = ArgText(argc, argv, "motion") == "none";   // motion=none: no motion vectors (all zero), to see what ours give
    if (count < 2) { printf("the recording has too few frames\n"); return 2; }
    std::wstring exeDir; { wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH); exeDir = exe; exeDir = exeDir.substr(0, exeDir.find_last_of(L'\\')); }

    // our side: a device on the same card, the shared frame in and picture out, two shared fences
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
    auto texture = [&](uint32_t tw, uint32_t th, bool uav, DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM) {
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
    ID3D12Resource* in = texture(w, hh, false, frameFmt); ID3D12Resource* out = texture(W, H, true, frameFmt);
    // oracle=1: the motion comes from the full-size frames (oracle_flow.h), not from our estimate: the flow the upscaler is given, as a texture
    const bool oracle = Arg(argc, argv, "oracle", 0) != 0;
    ID3D12Resource* flowTex = oracle ? texture(w, hh, false, DXGI_FORMAT_R16G16B16A16_FLOAT) : nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT flowFp{}; UINT64 flowTotal = 0;
    if (oracle) { D3D12_RESOURCE_DESC fd = flowTex->GetDesc(); UINT r2 = 0; UINT64 rb2 = 0; dev->GetCopyableFootprints(&fd, 0, 1, 0, &flowFp, &r2, &rb2, &flowTotal); }
    ID3D12Fence* copied = nullptr; ID3D12Fence* done = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&copied)); dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&done));
    if (!in || !out || !copied || !done) { printf("the shared textures or fences could not be made\n"); return 4; }
    D3D12_RESOURCE_DESC inDesc = in->GetDesc(), outDesc = out->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT inFp{}, outFp{}; UINT rows = 0; UINT64 rowBytes = 0, inTotal = 0, outTotal = 0;
    dev->GetCopyableFootprints(&inDesc, 0, 1, 0, &inFp, &rows, &rowBytes, &inTotal); dev->GetCopyableFootprints(&outDesc, 0, 1, 0, &outFp, &rows, &rowBytes, &outTotal);
    ID3D12Resource* upload = buffer(inTotal, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* flowUpload = oracle ? buffer(flowTotal, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ) : nullptr;
    ID3D12Resource* readback = buffer(outTotal, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

    // the engine, as the addon starts it (its own device on the same card), given our shared resources
    SrEngine eng;
    const std::wstring runtimeDir = exeDir + (backend == SrEngine::Backend::Fsr ? L"\\fsr" : backend == SrEngine::Backend::Xess ? L"\\xess" : L"\\dlss");
    if (const int nrFirst = Arg(argc, argv, "ngxfirst", 0); nrFirst != 0) {
        // Neural Rendering started first, as it does when both addons are on (issue #7): NVIDIA's NGX core is one per process and keeps the search
        // paths of its first Init. ngxfirst=2: Neural Rendering's list as it was (Lossless Scaling's folder and its own: the upscaler then fails
        // with FeatureNotFound); ngxfirst=1: the list as it is now (nr::ngxpaths::SearchList, with the DLSS runtime's folder the DLSS Upscaler
        // published when it loaded).
        std::vector<std::wstring> nrList = { exeDir, exeDir };   // (this program sits in the folder the addon's files are in)
        if (nrFirst == 1) { nr::ngxpaths::PublishDlssRuntime(exeDir + L"\\dlss"); nrList = nr::ngxpaths::SearchList(nrList); }
        const std::vector<const wchar_t*> nrPaths = nr::ngxpaths::AsArray(nrList);
        NVSDK_NGX_FeatureCommonInfo nrInfo{}; nrInfo.PathListInfo.Path = nrPaths.data(); nrInfo.PathListInfo.Length = static_cast<unsigned>(nrPaths.size());
        wchar_t tmpDir[MAX_PATH] = {}; GetTempPathW(MAX_PATH, tmpDir);
        const NVSDK_NGX_Result nr = NVSDK_NGX_D3D12_Init(0x24480451ull, (std::wstring(tmpDir) + L"DLSS5NR01_sreval").c_str(), dev, &nrInfo, NVSDK_NGX_Version_API);
        printf("Neural Rendering's NGX init first: %s\n", NVSDK_NGX_FAILED(nr) ? "failed" : "ok");
    }
    const uint64_t vramBefore = nr::ProcessVideoMemoryMb(ad.AdapterLuid);   // (with the shared textures of this program, which stand for Lossless Scaling's own)
    if (!eng.Init(ad.AdapterLuid, exeDir, runtimeDir, [](const char* m) { printf("  %s\n", m); }, backend)) { printf("the upscaler could not start: %s\n", eng.LastError().c_str()); return 4; }
    static int s_debugMessages = 0;
    if (debugLayer && eng.Device()) {
        ID3D12InfoQueue1* iq = nullptr; DWORD cookie = 0;
        if (SUCCEEDED(eng.Device()->QueryInterface(IID_PPV_ARGS(&iq)))) {
            iq->RegisterMessageCallback([](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID id, LPCSTR text, void*) {
                if (sev <= D3D12_MESSAGE_SEVERITY_WARNING) { ++s_debugMessages; printf("  D3D12 %s (%d): %s\n", sev == D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" : sev == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" : "warning", static_cast<int>(id), text); }
            }, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
            printf("the D3D12 debug layer is on for the engine's device\n");
        }
    }
    if (const int cap = Arg(argc, argv, "straycap", -1); cap >= 0) eng.SetStrayCap(cap == 0 ? 1e9f : static_cast<float>(cap));
    if (const int mw = Arg(argc, argv, "meanweight", -1); mw >= 0) eng.SetMeanWeight(mw / 100.0f);   // meanweight=N: percent (100: the plain difference)
    if (const int gw = Arg(argc, argv, "gradweight", -1); gw >= 0) eng.SetGradWeight(gw / 100.0f);   // gradweight=N: percent the edges count (0: the plain difference)
    if ((Arg(argc, argv, "meanweight", -1) >= 0 || Arg(argc, argv, "gradweight", -1) > 0) && Arg(argc, argv, "nowait", 0) == 0) eng.PrepareShapeCost();   // (the shape cost is a shader variant, made on a thread of its own live)
    if (const int rest = Arg(argc, argv, "restmix", -1); rest >= 0) eng.SetLeanRest(rest / 100.0f);   // restmix=N: percent of the plain resample kept even at rest
    if (const int steady = Arg(argc, argv, "steady", -1); steady >= 0) eng.SetSteadySharpen(steady / 100.0f);   // steady=N: percent the sharpening is cut where the picture shimmers
    if (const int a = Arg(argc, argv, "steadymv", -1); a >= 0) eng.SetSteadyMotion(a / 10.0f, Arg(argc, argv, "steadymv2", 60) / 10.0f);   // steadymv=A steadymv2=B: tenths of an output pixel of motion
    if (const int reuse = Arg(argc, argv, "flowreuse", 0); reuse > 0) eng.SetFlowReuse(reuse);   // flowreuse=1|2: every other frame keeps the last estimate (2) or refines its blocks per pixel (1)
    if (const int cut = Arg(argc, argv, "movecut", 0); cut > 0) eng.SetMoveCut(cut / 100.0f);   // movecut=N: percent the sharpening is cut in fast motion
    if (Arg(argc, argv, "fsrown", 1) == 0) eng.SetFsrOwnSharpen(false);   // fsrown=0: FSR's sharpening by AMD's RCAS, not our pass
    if (Arg(argc, argv, "steadysign", -1) > 0) eng.SetSteadySign(1.0f);                                       // steadysign=1: fetch the history the other way along the motion (default -1)
    if (ArgText(argc, argv, "lean") == "easu") eng.SetLeanMode(1);   // lean=easu: the lean blends toward FSR 1's EASU of the frame (default: Catmull-Rom)
    if (const int st = Arg(argc, argv, "stability", -1); st >= 0) eng.SetStability(st / 100.0f);   // stability=N: percent (the slider)
    eng.SetMotionScale(Arg(argc, argv, "mvscale", 100) / 100.0f);
    eng.SetNoMask(Arg(argc, argv, "mask", 1) == 0);
    if (const int fast = Arg(argc, argv, "fast", -1); fast >= 0) eng.SetFastMotion(static_cast<float>(fast));
    if (const int fastp = Arg(argc, argv, "fastp", -1); fastp >= 0) eng.SetFastMotion(fastp / 100.0f);   // fastp=N: the lean from N hundredths of a pixel (fast= is whole pixels; 0 there is off)
    auto share = [&](ID3D12DeviceChild* obj) { HANDLE sh = nullptr; dev->CreateSharedHandle(obj, nullptr, GENERIC_ALL, nullptr, &sh); return sh; };
    HANDLE hIn = share(in), hOut = share(out), hCopied = share(copied), hDone = share(done);
    ID3D12Resource* inE = eng.OpenSharedTexture(hIn); ID3D12Resource* outE = eng.OpenSharedTexture(hOut);
    ID3D12Fence* copiedE = eng.OpenSharedFence(hCopied); ID3D12Fence* doneE = eng.OpenSharedFence(hDone);
    CloseHandle(hIn); CloseHandle(hOut); CloseHandle(hCopied); CloseHandle(hDone);
    ID3D12Resource* flowE = oracle ? eng.OpenSharedTexture(share(flowTex)) : nullptr;
    if (!inE || !outE || !copiedE || !doneE || (oracle && !flowE)) { printf("the engine could not open the shared resources\n"); return 4; }
    printf("%ls: %ux%u, %d frames from %d, shrunk to %ux%u and upscaled back by %s\n", Wide(argv[1]).c_str(), W, H, count, first, w, hh, backendName.c_str());

    struct Score { int frame; double up, plain, upCoarse, plainCoarse; std::vector<uint8_t> truth, picture, stretched; };
    std::vector<Score> scores;
    std::vector<uint8_t> px, frame, shrunk, picture, stretched, framePrev, picturePrev, stretchedPrev, framePrev2, picturePrev2, stretchedPrev2;
    double flickOut = 0, flickIn = 0, detailOut = 0, detailIn = 0, flickStretch = 0, detailStretch = 0; int flickN = 0, detailN = 0;
    double sumUp = 0, sumPlain = 0, sumUpC = 0, sumPlainC = 0, sumUpT = 0, sumPlainT = 0; int n = 0;
    // hdr=1 helpers: light -> 8-bit (the roll-off and sRGB of ToRgba8), and the checks HDR needs
    auto tone = [](float v) { v = std::max(v, 0.0f); if (v > 0.75f) v = 0.75f + 0.25f * (1.0f - std::exp(-(v - 0.75f) / 0.25f)); v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    std::vector<float> frameF, shrunkF; double hdrMeanIn = 0, hdrMeanOut = 0; float hdrMaxIn = 0, hdrMaxOut = 0; uint64_t hdrBad = 0; int hdrN = 0;
    for (int i = 0; i < count; ++i) {
        if (!(i >= spliceAt && !splicePath.empty() ? rec2.Read(std::min<size_t>(rec2.Count() - 1, static_cast<size_t>(first + (i - spliceAt))), px) : rec.Read(Arg(argc, argv, "still", 0) != 0 ? first : first + i, px)) || !ToRgba8(h, px, frame)) { printf("frame %d could not be read\n", first + i); return 3; }
        if (pointSampled) {   // sample=point: what a game without anti-aliasing renders at a lower size (one sample at each pixel's centre): aliased, so a temporal upscaler has detail to unfold
            shrunk.assign(static_cast<size_t>(w) * hh * 4, 255);
            for (uint32_t y = 0; y < hh; ++y) for (uint32_t x = 0; x < w; ++x) {
                const uint32_t sx = std::min(W - 1, static_cast<uint32_t>((x + 0.5) * W / w)), sy = std::min(H - 1, static_cast<uint32_t>((y + 0.5) * H / hh));
                memcpy(&shrunk[(static_cast<size_t>(y) * w + x) * 4], &frame[(static_cast<size_t>(sy) * W + sx) * 4], 4);
            }
        } else ResizeArea(frame, W, H, shrunk, w, hh);
        if (hdrMode) {   // the frame as light (float), shrunk by area averaging, as half floats into the upload buffer; the plain stretch made in float too
            const size_t nPix = static_cast<size_t>(W) * H;
            frameF.resize(nPix * 3);
            { const uint16_t* hp = reinterpret_cast<const uint16_t*>(px.data());
              for (size_t k = 0; k < nPix; ++k) for (int c = 0; c < 3; ++c) frameF[k * 3 + c] = DirectX::PackedVector::XMConvertHalfToFloat(hp[k * 4 + c]) * hdrScale; }
            shrunkF.assign(static_cast<size_t>(w) * hh * 3, 0.0f);
            for (uint32_t y = 0; y < hh; ++y) for (uint32_t x = 0; x < w; ++x) {
                const uint32_t x0 = static_cast<uint32_t>(static_cast<uint64_t>(x) * W / w), x1 = std::max(x0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * W / w));
                const uint32_t y0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * H / hh), y1 = std::max(y0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * H / hh));
                double a[3] = { 0, 0, 0 }; int cnt = 0;
                for (uint32_t yy = y0; yy < std::min(y1, H); ++yy) for (uint32_t xx = x0; xx < std::min(x1, W); ++xx) { for (int c = 0; c < 3; ++c) a[c] += frameF[(static_cast<size_t>(yy) * W + xx) * 3 + c]; ++cnt; }
                for (int c = 0; c < 3; ++c) shrunkF[(static_cast<size_t>(y) * w + x) * 3 + c] = static_cast<float>(a[c] / std::max(cnt, 1));
            }
            uint8_t* m = nullptr; upload->Map(0, nullptr, reinterpret_cast<void**>(&m));
            for (uint32_t y = 0; y < hh; ++y) {
                uint16_t* row = reinterpret_cast<uint16_t*>(m + inFp.Offset + y * inFp.Footprint.RowPitch);
                for (uint32_t x = 0; x < w; ++x) { for (int c = 0; c < 3; ++c) row[x * 4 + c] = FloatToHalf(shrunkF[(static_cast<size_t>(y) * w + x) * 3 + c]); row[x * 4 + 3] = FloatToHalf(1.0f); }
            }
            upload->Unmap(0, nullptr);
            stretched.resize(nPix * 4);   // the plain stretch (bilinear, in light), toned down
            for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {
                const float fx = std::clamp((x + 0.5f) * w / W - 0.5f, 0.0f, static_cast<float>(w - 1)), fy = std::clamp((y + 0.5f) * hh / H - 0.5f, 0.0f, static_cast<float>(hh - 1));
                const uint32_t ix = static_cast<uint32_t>(fx), iy = static_cast<uint32_t>(fy), jx = std::min(ix + 1, w - 1), jy = std::min(iy + 1, hh - 1); const float ax = fx - ix, ay = fy - iy;
                for (int c = 0; c < 3; ++c) {
                    const float v = (shrunkF[(static_cast<size_t>(iy) * w + ix) * 3 + c] * (1 - ax) + shrunkF[(static_cast<size_t>(iy) * w + jx) * 3 + c] * ax) * (1 - ay)
                                  + (shrunkF[(static_cast<size_t>(jy) * w + ix) * 3 + c] * (1 - ax) + shrunkF[(static_cast<size_t>(jy) * w + jx) * 3 + c] * ax) * ay;
                    stretched[(static_cast<size_t>(y) * W + x) * 4 + c] = tone(v);
                }
                stretched[(static_cast<size_t>(y) * W + x) * 4 + 3] = 255;
            }
        } else {
        { uint8_t* m = nullptr; upload->Map(0, nullptr, reinterpret_cast<void**>(&m));
          for (uint32_t y = 0; y < hh; ++y) memcpy(m + inFp.Offset + y * inFp.Footprint.RowPitch, shrunk.data() + static_cast<size_t>(y) * w * 4, w * 4);
          upload->Unmap(0, nullptr); }
        }
        // realonly=1: what frame generation could do (the recording's frames are the presented ones): the upscaler runs on the even frames only (it is given nothing of the odd ones), the odd
        // ones are the plain Catmull-Rom stretch of their own frame; scored as they are, with the alternation counted by the steady and flicker numbers
        // realonly=1: the engine's own cheap run on the odd frames (SrEngine::SetCheapNext: no estimate, no upscaler; the plain stretch, sharpened); realonly=2: the plain stretch on the CPU, not sharpened
        if (i == 0) eng.SetWarpSign(static_cast<float>(Arg(argc, argv, "warpsign", 1)));
        if (Arg(argc, argv, "realonly", 0) == 1 && (i & 1)) eng.SetCheapNext(true);
        if (i == 0 && Arg(argc, argv, "realonly", 0) == 1) eng.SetPresentStep(Arg(argc, argv, "step", 50) / 100.0f);   // step=N: the share of a real frame's step one presented frame is (50: x2)
        const bool cheap = Arg(argc, argv, "realonly", 0) == 2 && (i & 1) && !hdrMode;
        if (!cheap) {
        alloc->Reset(); list->Reset(alloc, nullptr);
        auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
            D3D12_RESOURCE_BARRIER br{}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b }; list->ResourceBarrier(1, &br);
        };
        barrier(in, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION to{ in, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; to.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION from{ upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; from.PlacedFootprint = inFp;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        barrier(in, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        if (oracle) {   // the motion between this full-size frame and the one before, for the upscaler's input size (zero for the first)
            std::vector<float> vec;
            if (i > 0 && !framePrev.empty()) nr::oracle::ToInputVectors(nr::oracle::Estimate(nr::oracle::FromRgba8(frame, W, H), nr::oracle::FromRgba8(framePrev, W, H)), W, H, w, hh, vec);
            else vec.assign(static_cast<size_t>(w) * hh * 2, 0.0f);
            const float sign = Arg(argc, argv, "oraclesign", 1) < 0 ? -1.0f : 1.0f;
            uint8_t* m = nullptr; flowUpload->Map(0, nullptr, reinterpret_cast<void**>(&m));
            for (uint32_t y = 0; y < hh; ++y) {
                uint16_t* row = reinterpret_cast<uint16_t*>(m + flowFp.Offset + y * flowFp.Footprint.RowPitch);
                for (uint32_t x = 0; x < w; ++x) { row[x * 4] = FloatToHalf(sign * vec[(static_cast<size_t>(y) * w + x) * 2]); row[x * 4 + 1] = FloatToHalf(sign * vec[(static_cast<size_t>(y) * w + x) * 2 + 1]); row[x * 4 + 2] = 0; row[x * 4 + 3] = 0; }
            }
            flowUpload->Unmap(0, nullptr);
            barrier(flowTex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION fto{ flowTex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; fto.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION ffrom{ flowUpload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; ffrom.PlacedFootprint = flowFp;
            list->CopyTextureRegion(&fto, 0, 0, 0, &ffrom, nullptr);
            barrier(flowTex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        }
        submit();
        queue->Signal(copied, static_cast<uint64_t>(i) + 1);
        // the engine's run, on this thread (live, the engine's own thread runs it): the motion measured from the frames, no sharpening
        eng.Run(inE, w, hh, frameFmt, outE, W, H, frameFmt, flowE, flowE ? w : 0, flowE ? hh : 0, flowE ? 1.0f : 0.0f, 1.0f, !noMotion && !flowE, static_cast<unsigned>(Arg(argc, argv, "preset", 0)), Arg(argc, argv, "sharpen", 0) / 100.0f, i == 0, hdrMode,
                copiedE, static_cast<uint64_t>(i) + 1, doneE, static_cast<uint64_t>(i) + 1);
        queue->Wait(done, static_cast<uint64_t>(i) + 1);
        alloc->Reset(); list->Reset(alloc, nullptr);
        barrier(out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION rt{ readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; rt.PlacedFootprint = outFp;
        D3D12_TEXTURE_COPY_LOCATION rf{ out, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; rf.SubresourceIndex = 0;
        list->CopyTextureRegion(&rt, 0, 0, 0, &rf, nullptr);
        barrier(out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        submit();
        picture.resize(static_cast<size_t>(W) * H * 4);
        if (hdrMode) {   // the fp16 light, checked (finite, its mean and its peak against the frame's) and toned down to be scored
            uint8_t* m = nullptr; readback->Map(0, nullptr, reinterpret_cast<void**>(&m));
            double meanOut = 0, meanIn = 0; float maxOut = 0, maxIn = 0;
            for (uint32_t y = 0; y < H; ++y) {
                const uint16_t* row = reinterpret_cast<const uint16_t*>(m + outFp.Offset + y * outFp.Footprint.RowPitch);
                for (uint32_t x = 0; x < W; ++x) for (int c = 0; c < 3; ++c) {
                    const float v = DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4 + c]);
                    if (!std::isfinite(v)) { ++hdrBad; picture[(static_cast<size_t>(y) * W + x) * 4 + c] = 0; continue; }
                    meanOut += v; maxOut = std::max(maxOut, v);
                    picture[(static_cast<size_t>(y) * W + x) * 4 + c] = tone(v);
                    const float f = frameF[(static_cast<size_t>(y) * W + x) * 3 + c]; meanIn += f; maxIn = std::max(maxIn, f);
                }
            }
            D3D12_RANGE none{ 0, 0 }; readback->Unmap(0, &none);
            const double nCh = static_cast<double>(W) * H * 3; if (i >= 8) { hdrMeanIn += meanIn / nCh; hdrMeanOut += meanOut / nCh; hdrMaxIn = std::max(hdrMaxIn, maxIn); hdrMaxOut = std::max(hdrMaxOut, maxOut); ++hdrN; }
        } else {
        { uint8_t* m = nullptr; readback->Map(0, nullptr, reinterpret_cast<void**>(&m));
          for (uint32_t y = 0; y < H; ++y) memcpy(picture.data() + static_cast<size_t>(y) * W * 4, m + outFp.Offset + y * outFp.Footprint.RowPitch, W * 4);
          D3D12_RANGE none{ 0, 0 }; readback->Unmap(0, &none); }
        }
        } else ResizeCubic(shrunk, w, hh, picture, W, H);
        for (size_t k = 3; k < picture.size(); k += 4) picture[k] = 255;
        if (!hdrMode) ResizeBilinear(shrunk, w, hh, stretched, W, H);
        if (const int saveFrame = Arg(argc, argv, "saveframe", -1); saveFrame >= 0 && first + i == saveFrame)   // saveframe=N: that frame as it is | upscaled | stretched, as frameN.bmp
            WriteBmp(outDir + L"\\frame" + std::to_wstring(saveFrame) + L".bmp", { &frame, &picture, &stretched }, W, H);
        Score s; s.frame = first + i;
        s.up = Psnr(picture, frame, nullptr); s.plain = Psnr(stretched, frame, nullptr);
        s.upCoarse = PsnrCoarse(picture, frame, W); s.plainCoarse = PsnrCoarse(stretched, frame, W);
        const double upT = i ? PsnrTemporal(picture, picturePrev, frame, framePrev) : 99.0, plainT = i ? PsnrTemporal(stretched, stretchedPrev, frame, framePrev) : 99.0;
        const double same = i ? Psnr(frame, framePrev, nullptr) : 0.0;   // the frame against the one before (99: a repeat)
        printf("  frame %4d  upscaled %5.2f dB (coarse %5.2f, steady %5.2f)   stretched %5.2f dB (coarse %5.2f, steady %5.2f)   vs before %5.2f\n", s.frame, s.up, s.upCoarse, upT, s.plain, s.plainCoarse, plainT, same);
        if (i >= 8) { sumUp += s.up; sumPlain += s.plain; sumUpC += s.upCoarse; sumPlainC += s.plainCoarse; sumUpT += upT; sumPlainT += plainT; ++n; }   // (after the history has built)
        // no-reference: the frame before's flicker (it needs this frame as the one after), and the detail of every picture, against the game's own frames
        if (i >= 2 && !picturePrev2.empty()) {
            if (i >= 10) { flickOut += Flicker(picturePrev2, picturePrev, picture, W); flickIn += Flicker(framePrev2, framePrev, frame, W); flickStretch += Flicker(stretchedPrev2, stretchedPrev, stretched, W); ++flickN; }
        }
        if (i >= 9) { detailOut += Detail(picture, W); detailIn += Detail(frame, W); detailStretch += Detail(stretched, W); ++detailN; }
        framePrev2 = framePrev; picturePrev2 = picturePrev; stretchedPrev2 = stretchedPrev;
        framePrev = frame; picturePrev = picture; stretchedPrev = stretched;
        if (show > 0) { s.truth = frame; s.picture = picture; s.stretched = stretched; }
        scores.push_back(std::move(s));
        if (show > 0 && scores.size() > 64) {   // keep the pictures of the worst only
            auto worst = std::max_element(scores.begin(), scores.end(), [](const Score& a, const Score& b) { return (a.truth.empty() ? -1e9 : a.up - a.plain) < (b.truth.empty() ? -1e9 : b.up - b.plain); });
            worst->truth.clear(); worst->truth.shrink_to_fit(); worst->picture.clear(); worst->picture.shrink_to_fit(); worst->stretched.clear(); worst->stretched.shrink_to_fit();
        }
    }
    { const uint64_t vramAfter = nr::ProcessVideoMemoryMb(ad.AdapterLuid); printf("video memory: this program holds %llu MB; the upscaler and its textures added %llu MB (from %llu MB before it started)\n", (unsigned long long)vramAfter, (unsigned long long)(vramAfter - std::min(vramAfter, vramBefore)), (unsigned long long)vramBefore); }
    // steady: the frame-to-frame change against the truth's own change (shimmer, crawling edges and flicker cost; softness alone does not)
    if (flickN && detailN) printf("no reference (levels of 255 on luma; the picture / the game's own frames): flicker %.3f / %.3f (%.0f %%), detail %.3f / %.3f (%.0f %%); the plain stretch: flicker %.3f, detail %.3f\n",
                                  flickOut / flickN, flickIn / flickN, 100.0 * flickOut / std::max(1e-9, flickIn), detailOut / detailN, detailIn / detailN, 100.0 * detailOut / std::max(1e-9, detailIn),
                                  flickStretch / flickN, detailStretch / detailN);
    if (n) printf("average over %d frames (after the first 8): upscaled %.2f dB (coarse %.2f, steady %.2f), stretched %.2f dB (coarse %.2f, steady %.2f)\n", n, sumUp / n, sumUpC / n, sumUpT / n, sumPlain / n, sumPlainC / n, sumPlainT / n);
    // bench=N: the last frame's picture run N more times back to back, without waiting between them, so the GPU stays busy at full clocks (the timings above
    // are taken with the GPU idling between frames and read several times too high); prints the GPU times the engine measured over those runs
    if (const int bench = Arg(argc, argv, "bench", 0); bench > 0) {
        FILETIME ct, et, kt0, ut0, kt1, ut1; GetThreadTimes(GetCurrentThread(), &ct, &et, &kt0, &ut0);   // the CPU this thread spends on the runs (not the time it waits)
        for (int j = 1; j <= bench; ++j) {
            const uint64_t v = static_cast<uint64_t>(count) + j;
            queue->Signal(copied, v);
            eng.Run(inE, w, hh, frameFmt, outE, W, H, frameFmt, flowE, flowE ? w : 0, flowE ? hh : 0, flowE ? 1.0f : 0.0f, 1.0f, !noMotion && !flowE, static_cast<unsigned>(Arg(argc, argv, "preset", 0)), Arg(argc, argv, "sharpen", 0) / 100.0f, false, hdrMode,
                    copiedE, v, doneE, v);
        }
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        done->SetEventOnCompletion(static_cast<uint64_t>(count) + bench, ev); WaitForSingleObject(ev, 60000); CloseHandle(ev);
        Sleep(50);
        GetThreadTimes(GetCurrentThread(), &ct, &et, &kt1, &ut1);
        const auto ticks = [](const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        printf("CPU per run %.3f ms (user %.3f, kernel %.3f); ", (ticks(kt1) - ticks(kt0) + ticks(ut1) - ticks(ut0)) / 1e4 / bench, (ticks(ut1) - ticks(ut0)) / 1e4 / bench, (ticks(kt1) - ticks(kt0)) / 1e4 / bench);
        printf("bench %d runs back to back: ", bench);
    }
    if (hdrMode && hdrN) printf("HDR (fp16 light): non-finite values %llu; mean light in %.4f out %.4f (out / in %.3f); peak light in %.2f out %.2f (out / in %.2f)\n", static_cast<unsigned long long>(hdrBad), hdrMeanIn / hdrN, hdrMeanOut / hdrN, hdrMeanOut / std::max(1e-9, hdrMeanIn), hdrMaxIn, hdrMaxOut, hdrMaxOut / std::max(1e-9f, hdrMaxIn));
    if (hdrMode && hdrN && Arg(argc, argv, "hdrcheck", 0) != 0) {   // hdrcheck=1: the test's verdict: nothing non-finite, the mean light kept within 3 %, and no highlight made much brighter (a speck)
        const bool ok = hdrBad == 0 && std::fabs(hdrMeanOut / std::max(1e-9, hdrMeanIn) - 1.0) < 0.03 && hdrMaxOut <= hdrMaxIn * 1.5f + 0.6f;
        printf("%s  the HDR checks (finite, mean light within 3 %%, no highlight above 1.5x + 0.6 of the input's peak)\n", ok ? "PASS" : "FAIL");
        if (!ok) return 1;
    }
    if (debugLayer) printf("D3D12 debug layer: %d messages (errors, corruption, warnings)\n", s_debugMessages);
    printf("GPU time per frame at the end: everything %.2f ms, the motion estimate %.2f ms, after the upscaler (lean, edges, sharpening) %.2f ms\n", eng.GpuMs(), eng.MotionMs(), eng.AfterMs());
    // the frames where the upscaler did worst against a plain stretch (its history hurt most)
    std::vector<const Score*> order; for (const Score& s : scores) if (!s.truth.empty() && s.frame - first >= 8) order.push_back(&s);
    std::sort(order.begin(), order.end(), [](const Score* a, const Score* b) { return a->up - a->plain < b->up - b->plain; });
    for (int k = 0; k < show && k < static_cast<int>(order.size()); ++k) {
        const Score& s = *order[k];
        wchar_t name[64]; swprintf(name, 64, L"\\worst%d_frame%05d.bmp", k + 1, s.frame);
        WriteBmp(outDir + name, { &s.truth, &s.picture, &s.stretched }, W, H);
        printf("  worst %d: frame %d, upscaled %.2f dB against stretched %.2f: %ls (the frame | upscaled | stretched)\n", k + 1, s.frame, s.up, s.plain, (outDir + name).c_str());
    }
    eng.Drain();
    SafeRelease(inE); SafeRelease(outE); SafeRelease(copiedE); SafeRelease(doneE);
    eng.Shutdown();
    return 0;
}
