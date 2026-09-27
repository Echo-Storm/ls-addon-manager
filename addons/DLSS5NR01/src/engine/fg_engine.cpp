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

struct FgEngine::Ffx { HMODULE module = nullptr; ffxFunctions fn{}; ffxContext ctx = nullptr; ffxConfigureDescFrameGeneration cfg{}; };

namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

void BarrierOn(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

uint32_t FfxFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    default: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    }
}
// FSR's output texture: the frames' format, but one that takes unordered access (not an sRGB one; the copy out is within the same family)
DXGI_FORMAT StorageFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? DXGI_FORMAT_B8G8R8A8_UNORM : f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM : f;
}

} // namespace

void FgEngine::Log(const char* fmt, ...) {
    if (!m_log) return;
    char text[512]; va_list a; va_start(a, fmt); vsnprintf(text, sizeof text, fmt, a); va_end(a);
    m_log(text);
}
bool FgEngine::Fail(const char* what) { m_error = what; Log("frame generation engine: %s", what); Shutdown(); m_error = what; return false; }
void FgEngine::Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) { BarrierOn(m_list, r, from, to); }
bool FgEngine::WaitFence(uint64_t value, DWORD ms) {
    if (!m_fence || m_fence->GetCompletedValue() >= value) return true;
    m_fence->SetEventOnCompletion(value, m_event);
    return WaitForSingleObject(m_event, ms) == WAIT_OBJECT_0;
}

bool FgEngine::Init(const LUID& card, const std::wstring& runtimeDll, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool hdr, LogFn log) {
    Shutdown();
    m_log = std::move(log); m_error.clear();
    m_card = card; m_w = w; m_h = h; m_fmt = fmt; m_hdr = hdr;
    IDXGIFactory4* factory = nullptr; IDXGIAdapter1* adapter = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return Fail("no DXGI factory");
    const HRESULT ha = factory->EnumAdapterByLuid(card, IID_PPV_ARGS(&adapter)); factory->Release();
    if (FAILED(ha)) return Fail("Lossless Scaling's graphics card was not found");
    const HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_dev)); adapter->Release();
    if (FAILED(hr)) return Fail("no Direct3D 12 device on Lossless Scaling's card");
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    bool made = SUCCEEDED(m_dev->CreateCommandQueue(&q, IID_PPV_ARGS(&m_queue))) && SUCCEEDED(m_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    for (auto*& a : m_alloc) made = made && SUCCEEDED(m_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
    made = made && SUCCEEDED(m_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc[0], nullptr, IID_PPV_ARGS(&m_list)));
    if (!made) return Fail("the Direct3D 12 queue could not be made");
    m_list->Close(); m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // the GPU's time for each frame between (two timestamps a slot), for pacing
    D3D12_QUERY_HEAP_DESC qh{}; qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count = 2 * kSlots;
    if (FAILED(m_dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_stamps)))) m_stamps = nullptr;
    if (m_stamps && FAILED(m_queue->GetTimestampFrequency(&m_stampFreq))) m_stampFreq = 0;

    // textures: the motion vectors and the distrust mask (the estimate's), a flat depth (Lossless Scaling has none), FSR's output
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
    m_made = texture(StorageFormat(fmt), true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (m_stamps) m_stampReadback = buffer(16 * kSlots, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!m_motion || !m_distrust || !m_depth || !m_made) return Fail("its textures could not be made");
    {   // the flat depth, once
        D3D12_RESOURCE_DESC dd = m_depth->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 row = 0, total = 0;
        m_dev->GetCopyableFootprints(&dd, 0, 1, 0, &fp, &rows, &row, &total);
        ID3D12Resource* up = buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!up) return Fail("its textures could not be made");
        uint8_t* m = nullptr; up->Map(0, nullptr, reinterpret_cast<void**>(&m));
        const std::vector<float> line(w, 0.5f);
        for (UINT y = 0; y < rows; ++y) memcpy(m + fp.Offset + y * fp.Footprint.RowPitch, line.data(), w * 4);
        up->Unmap(0, nullptr);
        m_alloc[0]->Reset(); m_list->Reset(m_alloc[0], nullptr);
        D3D12_TEXTURE_COPY_LOCATION to{ m_depth, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; to.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION from{ up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; from.PlacedFootprint = fp;
        m_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        Barrier(m_depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_list->Close(); ID3D12CommandList* l[] = { m_list }; m_queue->ExecuteCommandLists(1, l);
        m_queue->Signal(m_fence, ++m_fenceValue);
        const bool ok = WaitFence(m_fenceValue, 5000); up->Release();
        if (!ok) return Fail("the GPU did not finish setting up");
    }
    if (!m_estimator.Init(m_dev, [this](const char* m) { Log("%s", m); }) || !m_estimator.Ensure(w, h)) return Fail("the motion estimate could not start");

    // AMD's runtime and the frame generation context, told to leave swap chains alone: we dispatch it ourselves
    m_ffx = new Ffx;
    m_ffx->module = LoadLibraryExW(runtimeDll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m_ffx->module) return Fail("AMD's FSR runtime could not be loaded");
    ffxLoadFunctions(&m_ffx->fn, m_ffx->module);
    if (!m_ffx->fn.CreateContext || !m_ffx->fn.Configure || !m_ffx->fn.Dispatch) return Fail("AMD's runtime lacks the FidelityFX API");
    ffxCreateBackendDX12Desc backend{}; backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; backend.device = m_dev;
    ffxCreateContextDescFrameGeneration create{}; create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION; create.header.pNext = &backend.header;
    create.flags = hdr ? static_cast<uint32_t>(FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE) : 0u;
    create.displaySize = { w, h }; create.maxRenderSize = { w, h }; create.backBufferFormat = FfxFormat(fmt);
    if (m_ffx->fn.CreateContext(&m_ffx->ctx, &create.header, nullptr) != FFX_API_RETURN_OK || !m_ffx->ctx) return Fail("FSR frame generation could not make its context");
    ffxConfigureDescFrameGeneration& cfg = m_ffx->cfg; cfg = {}; cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = nullptr; cfg.frameGenerationEnabled = true; cfg.allowAsyncWorkloads = false; cfg.onlyPresentGenerated = false;
    cfg.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    cfg.generationRect = { 0, 0, static_cast<int>(w), static_cast<int>(h) };
    if (m_ffx->fn.Configure(&m_ffx->ctx, &cfg.header) != FFX_API_RETURN_OK) return Fail("FSR frame generation could not be configured");
    m_frameId = 0; m_runs = 0; m_gpuMs = 0; m_ready = true;
    Log("frame generation engine: FSR 3.1 frame generation ready, %ux%u, format %d%s (dispatched directly)", w, h, static_cast<int>(fmt), hdr ? ", HDR" : "");
    return true;
}

void FgEngine::Shutdown() {
    if (m_queue && m_fence) { m_queue->Signal(m_fence, ++m_fenceValue); WaitFence(m_fenceValue, 2000); }
    if (m_ffx) {
        if (m_ffx->ctx && m_ffx->fn.DestroyContext) m_ffx->fn.DestroyContext(&m_ffx->ctx, nullptr);
        if (m_ffx->module) FreeLibrary(m_ffx->module);
        delete m_ffx; m_ffx = nullptr;
    }
    m_estimator.Shutdown();
    SafeRelease(m_motion); SafeRelease(m_distrust); SafeRelease(m_depth); SafeRelease(m_made);
    SafeRelease(m_stamps); SafeRelease(m_stampReadback);
    SafeRelease(m_list); for (auto*& a : m_alloc) SafeRelease(a);
    SafeRelease(m_fence); SafeRelease(m_queue); SafeRelease(m_dev);
    if (m_event) { CloseHandle(m_event); m_event = nullptr; }
    for (int i = 0; i < kSlots; ++i) { m_slotValue[i] = 0; m_slotRead[i] = true; }
    m_ready = false;
}

ID3D12Resource* FgEngine::OpenSharedTexture(HANDLE h) { ID3D12Resource* r = nullptr; if (m_dev) m_dev->OpenSharedHandle(h, IID_PPV_ARGS(&r)); return r; }
ID3D12Fence* FgEngine::OpenSharedFence(HANDLE h) { ID3D12Fence* f = nullptr; if (m_dev) m_dev->OpenSharedHandle(h, IID_PPV_ARGS(&f)); return f; }

void FgEngine::ReadSlot(int slot) {
    if (m_slotRead[slot] || !m_slotValue[slot] || m_fence->GetCompletedValue() < m_slotValue[slot]) return;
    m_slotRead[slot] = true;
    if (m_stampReadback && m_stampFreq) {
        const D3D12_RANGE range{ static_cast<SIZE_T>(slot) * 16, static_cast<SIZE_T>(slot) * 16 + 16 };
        uint64_t* t = nullptr;
        if (SUCCEEDED(m_stampReadback->Map(0, &range, reinterpret_cast<void**>(&t)))) {
            const uint64_t a = t[slot * 2], b = t[slot * 2 + 1];
            if (b > a) { const double ms = (b - a) * 1000.0 / static_cast<double>(m_stampFreq); m_gpuMs = m_gpuMs > 0 ? 0.9 * m_gpuMs + 0.1 * ms : ms; }
            const D3D12_RANGE none{ 0, 0 }; m_stampReadback->Unmap(0, &none);
        }
    }
    m_estimator.ReadStats(slot);   // this frame's motion joins the averages, logged every 600 frames
    if (m_runs % 600 == 599) {
        double x = 0, y = 0, length = 0, cost = 0, distrust = 0; uint64_t frames = 0;
        if (m_estimator.TakeAverages(x, y, length, cost, distrust, frames))
            Log("frame generation engine: motion over %llu frames, average vector (%.1f, %.1f) px, average length %.1f px, match cost %.4f; the GPU %.2f ms a frame",
                static_cast<unsigned long long>(frames), x, y, length, cost, m_gpuMs);
    }
}

bool FgEngine::Generate(ID3D12Resource* in, ID3D12Fence* copied, uint64_t n, ID3D12Resource* out, ID3D12Fence* made, uint64_t madeValue, float frameMs, bool reset,
                        uint32_t encoding, float whiteNits) {
    if (!m_ready) return false;
    for (int s = 0; s < kSlots; ++s) ReadSlot(s);   // what has finished since
    const int slot = static_cast<int>(m_runs % kSlots);
    if (!WaitFence(m_slotValue[slot], 100)) {   // (its work from kSlots frames ago: long done, unless the GPU is swamped)
        Log("frame generation engine: the GPU is %d frames behind; this frame gets none between", kSlots);
        m_queue->Signal(made, madeValue);
        return false;
    }
    ReadSlot(slot);
    if (reset) m_estimator.Forget();
    m_alloc[slot]->Reset(); m_list->Reset(m_alloc[slot], nullptr);
    if (m_stamps) m_list->EndQuery(m_stamps, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot * 2));
    // the frame's motion (the estimate keeps the frame before), then FSR's preparation and its frame between, on this list
    Barrier(in, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_estimator.Record(m_list, slot, in, m_fmt, m_motion, m_distrust, 0.0f, encoding, whiteNits);
    Barrier(m_motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const bool first = reset || m_frameId == 0;
    ffxConfigureDescFrameGeneration& cfg = m_ffx->cfg;
    cfg.frameID = m_frameId;   // configure, prepare and dispatch with the same frame ID (else FSR takes it as a reset)
    bool ok = m_ffx->fn.Configure(&m_ffx->ctx, &cfg.header) == FFX_API_RETURN_OK;
    if (ok) {
        ffxDispatchDescFrameGenerationPrepare prep{}; prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        prep.frameID = m_frameId; prep.commandList = m_list; prep.renderSize = { m_w, m_h }; prep.jitterOffset = { 0, 0 }; prep.motionVectorScale = { 1.0f, 1.0f };
        prep.frameTimeDelta = frameMs; prep.cameraNear = 0.1f; prep.cameraFar = 1000.0f; prep.cameraFovAngleVertical = 1.0f; prep.viewSpaceToMetersFactor = 1.0f;
        prep.depth = ffxApiGetResourceDX12(m_depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        prep.motionVectors = ffxApiGetResourceDX12(m_motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        ok = m_ffx->fn.Dispatch(&m_ffx->ctx, &prep.header) == FFX_API_RETURN_OK;
        if (!ok) Log("frame generation engine: FSR's preparation failed");
    } else Log("frame generation engine: FSR's configure failed");
    if (ok) {
        ffxDispatchDescFrameGeneration gen{}; gen.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        gen.commandList = m_list; gen.presentColor = ffxApiGetResourceDX12(in, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        gen.outputs[0] = ffxApiGetResourceDX12(m_made, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS); gen.numGeneratedFrames = 1; gen.reset = first;
        gen.backbufferTransferFunction = !m_hdr ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB
                                       : m_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ;
        gen.minMaxLuminance[0] = 0.0f; gen.minMaxLuminance[1] = 0.0f;   // (as FidelityFX's swap chain gives them without HDR metadata)
        gen.generationRect = { 0, 0, static_cast<int>(m_w), static_cast<int>(m_h) }; gen.frameID = m_frameId;
        ok = m_ffx->fn.Dispatch(&m_ffx->ctx, &gen.header) == FFX_API_RETURN_OK;
        if (!ok) Log("frame generation engine: FSR's frame generation failed");
    }
    const bool produced = ok && !first;
    if (produced) {   // the frame between into the caller's shared texture
        Barrier(m_made, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        m_list->CopyResource(out, m_made);
        Barrier(out, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Barrier(m_made, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    Barrier(in, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(m_motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (m_stamps) {
        m_list->EndQuery(m_stamps, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot * 2 + 1));
        if (m_stampReadback) m_list->ResolveQueryData(m_stamps, D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot * 2), 2, m_stampReadback, static_cast<UINT64>(slot) * 16);
    }
    m_list->Close();
    m_queue->Wait(copied, n);   // Lossless Scaling's copy of the frame first
    ID3D12CommandList* l[] = { m_list }; m_queue->ExecuteCommandLists(1, l);
    m_queue->Signal(made, madeValue);
    m_queue->Signal(m_fence, ++m_fenceValue);
    m_slotValue[slot] = m_fenceValue; m_slotRead[slot] = false;
    ++m_frameId; ++m_runs;
    return produced;
}

} // namespace nr
