#include "engine/fg_engine.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>
#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_framegeneration.h"
#include "ffx_api/ffx_api_loader.h"
#include "ffx_api/dx12/ffx_api_dx12.h"

namespace nr {

struct FgEngine::Ffx { HMODULE module = nullptr; ffxFunctions fn{}; ffxContext chainCtx = nullptr; ffxContext ctx = nullptr; ffxConfigureDescFrameGeneration cfg{}; };

namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// The generation callback's view of the engine (one engine at a time): where the generated frame goes, and the marker copied after it.
struct Callback {
    ffxFunctions* fn = nullptr; ffxContext* ctx = nullptr;
    ID3D12Resource* out = nullptr; ID3D12Resource* marker = nullptr; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    volatile LONG generated = 0;
} g_cb;

const uint8_t kSentinel = 0xA5;
const UINT kMarkerTexels = 4, kMarkerPitch = 256;

D3D12_RESOURCE_STATES StateOf(uint32_t ffxState) {
    return ffxState & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
         : ffxState & FFX_API_RESOURCE_STATE_COPY_DEST ? D3D12_RESOURCE_STATE_COPY_DEST
         : ffxState & FFX_API_RESOURCE_STATE_COMPUTE_READ ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;
}
void BarrierOn(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}
UINT BytesPerTexel(DXGI_FORMAT f) { return f == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u; }

// FSR hands the frame it made to this, on its own command list: the frame goes into `out` (shared, left in COMMON for Lossless Scaling's
// device), then a few of its texels into the marker, which the engine's thread watches for.
ffxReturnCode_t OnGenerate(ffxDispatchDescFrameGeneration* params, void* user) {
    const ffxReturnCode_t rc = g_cb.fn->Dispatch(static_cast<ffxContext*>(user), &params->header);
    auto* list = static_cast<ID3D12GraphicsCommandList*>(params->commandList);
    auto* made = static_cast<ID3D12Resource*>(params->outputs[0].resource);
    if (rc != FFX_API_RETURN_OK || !list || !made || !g_cb.out) return rc;
    const D3D12_RESOURCE_STATES was = StateOf(params->outputs[0].state);
    BarrierOn(list, made, was, D3D12_RESOURCE_STATE_COPY_SOURCE);
    BarrierOn(list, g_cb.out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(g_cb.out, made);
    BarrierOn(list, g_cb.out, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);   // (the marker is read from `out`, after the copy into it)
    D3D12_TEXTURE_COPY_LOCATION to{ g_cb.marker, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    to.PlacedFootprint.Footprint = { g_cb.fmt, kMarkerTexels, 1, 1, kMarkerPitch };
    D3D12_TEXTURE_COPY_LOCATION from{ g_cb.out, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; from.SubresourceIndex = 0;
    const D3D12_BOX box{ 0, 0, 0, kMarkerTexels, 1, 1 };
    list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
    BarrierOn(list, g_cb.out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    BarrierOn(list, made, D3D12_RESOURCE_STATE_COPY_SOURCE, was);
    InterlockedIncrement(&g_cb.generated);
    return rc;
}

uint32_t FfxFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    default: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    }
}

} // namespace

void FgEngine::Log(const char* fmt, ...) {
    if (!m_log) return;
    char text[512]; va_list a; va_start(a, fmt); vsnprintf(text, sizeof text, fmt, a); va_end(a);
    m_log(text);
}
bool FgEngine::Fail(const char* what) { m_error = what; Log("frame generation engine: %s", what); Shutdown(); m_error = what; return false; }
void FgEngine::Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) { BarrierOn(m_list, r, from, to); }
bool FgEngine::Submit(DWORD waitMs) {
    m_list->Close(); ID3D12CommandList* l[] = { m_list }; m_queue->ExecuteCommandLists(1, l);
    m_queue->Signal(m_fence, ++m_fenceValue);
    if (m_fence->GetCompletedValue() >= m_fenceValue) return true;
    m_fence->SetEventOnCompletion(m_fenceValue, m_event);
    return WaitForSingleObject(m_event, waitMs) == WAIT_OBJECT_0;
}

bool FgEngine::Init(const LUID& card, const std::wstring& runtimeDll, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool hdr, float whiteNits, LogFn log) {
    Shutdown();
    m_log = std::move(log); m_error.clear();
    m_card = card; m_w = w; m_h = h; m_fmt = fmt; m_hdr = hdr; m_white = whiteNits > 1.0f ? whiteNits : 200.0f;
    IDXGIFactory4* factory = nullptr; IDXGIAdapter1* adapter = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return Fail("no DXGI factory");
    if (FAILED(factory->EnumAdapterByLuid(card, IID_PPV_ARGS(&adapter)))) { factory->Release(); return Fail("Lossless Scaling's graphics card was not found"); }
    const HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_dev)); adapter->Release();
    if (FAILED(hr)) { factory->Release(); return Fail("no Direct3D 12 device on Lossless Scaling's card"); }
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(m_dev->CreateCommandQueue(&q, IID_PPV_ARGS(&m_queue))) || FAILED(m_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc))) ||
        FAILED(m_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc, nullptr, IID_PPV_ARGS(&m_list))) || FAILED(m_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) {
        factory->Release(); return Fail("the Direct3D 12 queue could not be made");
    }
    m_list->Close(); m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // textures: the motion vectors and the distrust mask (the estimate's), a flat depth (Lossless Scaling has none), the marker
    auto texture = [&](DXGI_FORMAT f, bool uav, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
        d.Format = f; d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        ID3D12Resource* r = nullptr; m_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)); return r;
    };
    auto buffer = [&](UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* r = nullptr; m_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)); return r;
    };
    m_motion = texture(DXGI_FORMAT_R16G16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_distrust = texture(DXGI_FORMAT_R8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_depth = texture(DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_COPY_DEST);
    m_marker = buffer(kMarkerPitch, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!m_motion || !m_distrust || !m_depth || !m_marker) { factory->Release(); return Fail("its textures could not be made"); }
    {   // the flat depth, once
        D3D12_RESOURCE_DESC dd = m_depth->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 row = 0, total = 0;
        m_dev->GetCopyableFootprints(&dd, 0, 1, 0, &fp, &rows, &row, &total);
        ID3D12Resource* up = buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* m = nullptr; up->Map(0, nullptr, reinterpret_cast<void**>(&m));
        const std::vector<float> line(w, 0.5f);
        for (UINT y = 0; y < rows; ++y) memcpy(m + fp.Offset + y * fp.Footprint.RowPitch, line.data(), w * 4);
        up->Unmap(0, nullptr);
        m_alloc->Reset(); m_list->Reset(m_alloc, nullptr);
        D3D12_TEXTURE_COPY_LOCATION to{ m_depth, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; to.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION from{ up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; from.PlacedFootprint = fp;
        m_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        Barrier(m_depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const bool ok = Submit(); up->Release();
        if (!ok) { factory->Release(); return Fail("the GPU did not finish setting up"); }
    }
    if (!m_estimator.Init(m_dev, [this](const char* m) { Log("%s", m); }) || !m_estimator.Ensure(w, h)) { factory->Release(); return Fail("the motion estimate could not start"); }

    // AMD's runtime, FidelityFX's swap chain on a window never shown, and the frame generation context
    m_ffx = new Ffx;
    m_ffx->module = LoadLibraryExW(runtimeDll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m_ffx->module) { factory->Release(); return Fail("AMD's FSR runtime could not be loaded"); }
    ffxLoadFunctions(&m_ffx->fn, m_ffx->module);
    if (!m_ffx->fn.CreateContext || !m_ffx->fn.Configure || !m_ffx->fn.Dispatch) { factory->Release(); return Fail("AMD's runtime lacks the FidelityFX API"); }
    WNDCLASSEXW wc{ sizeof wc }; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"EchoFrameGen"; RegisterClassExW(&wc);
    m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"frame generation", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = w; sd.Height = h; sd.Format = fmt; sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 scd{}; scd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    scd.swapchain = &m_chain; scd.hwnd = m_hwnd; scd.desc = &sd; scd.fullscreenDesc = nullptr; scd.dxgiFactory = factory; scd.gameQueue = m_queue;
    const ffxReturnCode_t rs = m_ffx->fn.CreateContext(&m_ffx->chainCtx, &scd.header, nullptr);
    factory->Release();
    if (rs != FFX_API_RETURN_OK || !m_chain) return Fail("FidelityFX's swap chain could not be made");
    if (hdr) m_chain->SetColorSpace1(fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
    ffxCreateBackendDX12Desc backend{}; backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; backend.device = m_dev;
    ffxCreateContextDescFrameGeneration create{}; create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION; create.header.pNext = &backend.header;
    create.flags = hdr ? static_cast<uint32_t>(FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE) : 0u;
    create.displaySize = { w, h }; create.maxRenderSize = { w, h }; create.backBufferFormat = FfxFormat(fmt);
    if (m_ffx->fn.CreateContext(&m_ffx->ctx, &create.header, nullptr) != FFX_API_RETURN_OK || !m_ffx->ctx) return Fail("FSR frame generation could not make its context");
    g_cb.fn = &m_ffx->fn; g_cb.ctx = &m_ffx->ctx; g_cb.marker = m_marker; g_cb.fmt = fmt; g_cb.out = nullptr;
    ffxConfigureDescFrameGeneration& cfg = m_ffx->cfg; cfg = {}; cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = m_chain; cfg.frameGenerationEnabled = true; cfg.allowAsyncWorkloads = false; cfg.onlyPresentGenerated = false;
    cfg.generationRect = { 0, 0, static_cast<int>(w), static_cast<int>(h) };
    cfg.frameGenerationCallback = OnGenerate; cfg.frameGenerationCallbackUserContext = &m_ffx->ctx;
    if (m_ffx->fn.Configure(&m_ffx->ctx, &cfg.header) != FFX_API_RETURN_OK) return Fail("FSR frame generation could not be configured");
    m_frameId = 0; m_ready = true;
    Log("frame generation engine: FSR 3.1 frame generation ready, %ux%u, format %d%s", w, h, static_cast<int>(fmt), hdr ? ", HDR" : "");
    return true;
}

void FgEngine::Shutdown() {
    if (m_queue && m_fence) { m_queue->Signal(m_fence, ++m_fenceValue); if (m_fence->GetCompletedValue() < m_fenceValue && m_event) { m_fence->SetEventOnCompletion(m_fenceValue, m_event); WaitForSingleObject(m_event, 2000); } }
    if (m_ffx) {
        if (m_ffx->ctx && m_ffx->fn.DestroyContext) m_ffx->fn.DestroyContext(&m_ffx->ctx, nullptr);
        if (m_chain) { m_chain->Release(); m_chain = nullptr; }
        if (m_ffx->chainCtx && m_ffx->fn.DestroyContext) m_ffx->fn.DestroyContext(&m_ffx->chainCtx, nullptr);
        if (m_ffx->module) FreeLibrary(m_ffx->module);
        delete m_ffx; m_ffx = nullptr;
    }
    SafeRelease(m_chain);
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
    g_cb = Callback{};
    m_estimator.Shutdown();
    SafeRelease(m_motion); SafeRelease(m_distrust); SafeRelease(m_depth); SafeRelease(m_marker);
    SafeRelease(m_list); SafeRelease(m_alloc); SafeRelease(m_fence); SafeRelease(m_queue); SafeRelease(m_dev);
    if (m_event) { CloseHandle(m_event); m_event = nullptr; }
    m_ready = false;
}

ID3D12Resource* FgEngine::OpenSharedTexture(HANDLE h) { ID3D12Resource* r = nullptr; if (m_dev) m_dev->OpenSharedHandle(h, IID_PPV_ARGS(&r)); return r; }
ID3D12Fence* FgEngine::OpenSharedFence(HANDLE h) { ID3D12Fence* f = nullptr; if (m_dev) m_dev->OpenSharedHandle(h, IID_PPV_ARGS(&f)); return f; }

bool FgEngine::Generate(ID3D12Resource* in, ID3D12Fence* copied, uint64_t n, ID3D12Resource* out, float frameMs, bool reset) {
    if (!m_ready) return false;
    if (reset) m_estimator.Forget();
    // the frame's motion (the estimate keeps the frame before), FSR's preparation, and the frame into FidelityFX's back buffer
    m_alloc->Reset(); m_list->Reset(m_alloc, nullptr);
    Barrier(in, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_estimator.Record(m_list, 0, in, m_fmt, m_motion, m_distrust, 0.0f, m_hdr ? 1u : 0u, m_white);
    Barrier(m_motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ffxDispatchDescFrameGenerationPrepare prep{}; prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prep.frameID = m_frameId; prep.commandList = m_list; prep.renderSize = { m_w, m_h }; prep.jitterOffset = { 0, 0 }; prep.motionVectorScale = { 1.0f, 1.0f };
    prep.frameTimeDelta = frameMs; prep.cameraNear = 0.1f; prep.cameraFar = 1000.0f; prep.cameraFovAngleVertical = 1.0f; prep.viewSpaceToMetersFactor = 1.0f;
    prep.depth = ffxApiGetResourceDX12(m_depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    prep.motionVectors = ffxApiGetResourceDX12(m_motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    if (m_ffx->fn.Dispatch(&m_ffx->ctx, &prep.header) != FFX_API_RETURN_OK) { m_list->Close(); Log("frame generation engine: FSR's preparation failed"); return false; }
    ID3D12Resource* back = nullptr; m_chain->GetBuffer(m_chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back));
    Barrier(in, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    m_list->CopyResource(back, in);
    Barrier(back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Barrier(in, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(m_motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_queue->Wait(copied, n);   // Lossless Scaling's copy of the frame first
    const bool submitted = Submit(); back->Release();
    if (!submitted) { Log("frame generation engine: the GPU did not finish the frame within 5 s"); return false; }
    m_estimator.ReadStats(0);   // the list has finished: this frame's motion joins the averages, logged every 600 frames
    if (m_frameId % 600 == 599) {
        double x = 0, y = 0, length = 0, cost = 0, distrust = 0; uint64_t frames = 0;
        if (m_estimator.TakeAverages(x, y, length, cost, distrust, frames))
            Log("frame generation engine: motion over %llu frames, average vector (%.1f, %.1f) px, average length %.1f px, match cost %.4f",
                static_cast<unsigned long long>(frames), x, y, length, cost);
    }

    // the marker reset, then the present: FSR makes the frame between the one before and this one, and OnGenerate copies it into `out`
    { uint8_t* m = nullptr; D3D12_RANGE all{ 0, kMarkerPitch }; m_marker->Map(0, &all, reinterpret_cast<void**>(&m)); memset(m, kSentinel, kMarkerPitch); m_marker->Unmap(0, &all); }
    g_cb.out = out;
    m_ffx->cfg.frameID = m_frameId++;
    m_ffx->fn.Configure(&m_ffx->ctx, &m_ffx->cfg.header);
    const LONG before = g_cb.generated;
    if (FAILED(m_chain->Present(0, 0))) { Log("frame generation engine: FidelityFX's present failed"); return false; }
    if (reset || m_frameId < 2) return false;
    const UINT bytes = kMarkerTexels * BytesPerTexel(m_fmt);
    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        bool arrived = g_cb.generated != before;
        if (arrived) {
            const uint8_t* m = nullptr; D3D12_RANGE r{ 0, bytes }; m_marker->Map(0, &r, (void**)&m);
            bool untouched = true; for (UINT i = 0; i < bytes; ++i) untouched = untouched && m[i] == kSentinel;
            D3D12_RANGE none{ 0, 0 }; m_marker->Unmap(0, &none);
            if (!untouched) return true;
        }
        if (GetTickCount64() - t0 > 100) { Log("frame generation engine: no frame between within 100 ms"); return false; }
        YieldProcessor();
    }
}

} // namespace nr
