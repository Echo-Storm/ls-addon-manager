#include "engine/nr_engine.h"
#include "engine/ngx_users.h"
#include <chrono>
#include "engine/nr_shaders.h"
#include "engine/dlaa_model.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "nvsdk_ngx.h"

namespace {

constexpr unsigned long long kAppId = 0x24480451ull;   // the application id NGX is started with (the same as the forwarder's)
constexpr DWORD kSlotWaitMs = 2000, kIdleWaitMs = 5000;
constexpr int kFloatGetterSlot = 14;                   // the parameter block's float getter (see nr_forwarder.cpp)
enum Pass { kShrink = 0, kMotion = 1, kDeltaPass = 2 };

// Root constants b0, twelve dwords, as the shaders declare them (nr_shaders.h).
struct PassConstants { uint32_t dstW, dstH, srcW, srcH; uint32_t flags; float flowScale; float smoothAmount; uint32_t encoding; float white; uint32_t pad[3]; };
static_assert(sizeof(PassConstants) == 48, "twelve root constants");

const char* NgxResultName(int r) {
    switch (static_cast<NVSDK_NGX_Result>(r)) {
    case NVSDK_NGX_Result_Success: return "Success";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "FeatureNotSupported";
    case NVSDK_NGX_Result_FAIL_PlatformError: return "PlatformError";
    case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "FeatureNotFound";
    case NVSDK_NGX_Result_FAIL_InvalidParameter: return "InvalidParameter";
    case NVSDK_NGX_Result_FAIL_NotInitialized: return "NotInitialized";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "UnsupportedInputFormat";
    case NVSDK_NGX_Result_FAIL_RWFlagMissing: return "RWFlagMissing";
    case NVSDK_NGX_Result_FAIL_MissingInput: return "MissingInput";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "UnableToInitializeFeature";
    case NVSDK_NGX_Result_FAIL_OutOfDate: return "OutOfDate";
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: return "OutOfGPUMemory";
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat: return "UnsupportedFormat";
    case NVSDK_NGX_Result_FAIL_Denied: return "Denied";
    default: return "Fail";
    }
}

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// NGX's log callback has no user pointer: it reaches the engine through this.
NrEngine* g_logTarget = nullptr;
void NVSDK_CONV OnNgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    if (!g_logTarget || !message) return;
    std::string line(message);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.find("NGXLoadConfig") != std::string::npos || line.find("NGXLoadFromPath") != std::string::npos) return;   // noise on every start
    if (line.find("error") != std::string::npos || line.find("warning") != std::string::npos || line.find("dlssnr") != std::string::npos)
        g_logTarget->Log("[ngx] %s", line.c_str());
}

} // namespace

void NrEngine::Log(const char* fmt, ...) {
    if (!m_log) return;
    char text[1024]; va_list args; va_start(args, fmt); vsnprintf(text, sizeof text, fmt, args); va_end(args);
    m_log(text);
}

void NrEngine::Fail(const char* fmt, ...) {
    va_list args; va_start(args, fmt); vsnprintf(m_stats.lastError, sizeof m_stats.lastError, fmt, args); va_end(args);
    m_failed = true; m_ready = false;
    Log("NrEngine FAILED: %s", m_stats.lastError);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Starting and stopping
// ---------------------------------------------------------------------------------------------------------------------------------

bool NrEngine::Init(const LUID& luid, const std::wstring& forwarderPath, const std::wstring& snippetPath, const std::wstring& dataPath,
                    const std::wstring& lsDir, LogFn log) {
    if (m_abandoned) { m_failed = true; return false; }   // its thread is still stuck in NVIDIA's code: only a restart of Lossless Scaling helps
    m_log = std::move(log); m_forwarderPath = forwarderPath; m_snippetPath = snippetPath; m_dataPath = dataPath; m_lsDir = lsDir;
    m_failed = m_ready = false;
    m_stats = NrStats{};
    g_logTarget = this;
    if (!CreateQueue(luid) || !StartNgx() || !StartModel() || !CreatePipelines()) return false;
    StartWorker();
    m_ready = true;
    Log("NrEngine ready (float slot %d)", m_stats.floatSlot);
    return true;
}

bool NrEngine::Shutdown() {
    m_ready = false;
    if (!StopWorker(kIdleWaitMs)) {   // its thread is inside NVIDIA's code and does not come back: nothing it may still use is torn down
        m_abandoned = true; m_failed = true;
        snprintf(m_stats.lastError, sizeof m_stats.lastError, "the model stopped responding; restart Lossless Scaling to use it again");
        Log("NrEngine: the engine's thread did not stop within %lu ms (stuck in the model's code); the engine is left as it is until Lossless Scaling closes", kIdleWaitMs);
        return false;
    }
    EndBuild();
    if (m_buildState.load() == kBuilt) Discard(m_built);
    m_buildState = kIdle;
    SafeRelease(m_buildList); SafeRelease(m_buildAlloc); SafeRelease(m_buildFence); SafeRelease(m_buildQueue);
    if (m_buildEvent) { CloseHandle(m_buildEvent); m_buildEvent = nullptr; }
    m_buildFenceValue = 0;
    if (m_queue && !WaitIdle()) {   // (the estimator's textures stay too: the GPU may still use them)
        m_ready = false; m_failed = true;
        Log("NrEngine: the GPU did not finish the model's work; the engine is left as it is until Lossless Scaling closes");
        return false;
    }
    ReleaseScratch();
    m_estimator.Shutdown(); m_estimatedLast = false; m_estimates = 0;
    dlaa::Shutdown();
    if (m_caps) { NVSDK_NGX_D3D12_DestroyParameters(static_cast<NVSDK_NGX_Parameter*>(m_caps)); m_caps = nullptr; }
    if (m_ngxJoined) {   // NGX shut down only by the last of our engines to use it (ngx_users.h): the DLSS Upscaler may still run
        m_ngxJoined = false;
        if (nr::ngxusers::Leaving()) NVSDK_NGX_D3D12_Shutdown1(m_dev); else Log("NrEngine: NGX left running (another addon still uses it)");
    }
    if (m_forwarder) { FreeLibrary(m_forwarder); m_forwarder = nullptr; }
    for (ID3D12PipelineState** p : { &m_psoShrink, &m_psoMotion, &m_psoDelta, &m_psoDeltaSmooth }) SafeRelease(*p);
    SafeRelease(m_rootSig); SafeRelease(m_heap); SafeRelease(m_timestamps); SafeRelease(m_timestampReadback); SafeRelease(m_list);
    for (ID3D12CommandAllocator*& a : m_alloc) SafeRelease(a);
    for (uint64_t& v : m_slotDone) v = 0;
    SafeRelease(m_fence);
    if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
    SafeRelease(m_queue); SafeRelease(m_dev);
    m_fenceValue = 0; m_nextSlot = 0; m_flow = nullptr; m_flowW = m_flowH = 0;
    m_ready = false;
    if (g_logTarget == this) g_logTarget = nullptr;
    return true;
}

bool NrEngine::CreateQueue(const LUID& luid) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) { Fail("CreateDXGIFactory1"); return false; }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; !adapter; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc; a->GetDesc1(&desc);
        if (desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart) adapter = a; else a->Release();
    }
    factory->Release();
    if (!adapter) { Fail("no graphics card with LUID %08x:%08x", luid.HighPart, luid.LowPart); return false; }
    const HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_dev));
    adapter->Release();
    if (FAILED(hr)) { Fail("D3D12CreateDevice 0x%08x", (unsigned)hr); return false; }

    D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(m_dev->CreateCommandQueue(&queue, IID_PPV_ARGS(&m_queue)))) { Fail("CreateCommandQueue"); return false; }
    for (ID3D12CommandAllocator*& a : m_alloc)
        if (FAILED(m_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) { Fail("CreateCommandAllocator"); return false; }
    if (FAILED(m_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc[0], nullptr, IID_PPV_ARGS(&m_list)))) { Fail("CreateCommandList"); return false; }
    m_list->Close();
    if (FAILED(m_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) { Fail("CreateFence"); return false; }
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // four timestamps a run (start, model start, model end, end), read back from 32 bytes a slot
    D3D12_QUERY_HEAP_DESC queries{}; queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; queries.Count = 4 * kSlots;
    m_dev->CreateQueryHeap(&queries, IID_PPV_ARGS(&m_timestamps));
    D3D12_HEAP_PROPERTIES readback{}; readback.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = 32 * kSlots; buffer.Height = 1;
    buffer.DepthOrArraySize = 1; buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    m_dev->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_timestampReadback));
    m_queue->GetTimestampFrequency(&m_timestampFreq);
    return true;
}

bool NrEngine::StartNgx() {
    // DLAA: NVIDIA's runtime ships in the addon's dlss folder, searched only then (NGX loads every runtime it finds on the way)
    const std::wstring dlssDir = m_dataPath + L"\\dlss";
    const wchar_t* searchPaths[] = { m_lsDir.c_str(), m_dataPath.c_str(), dlssDir.c_str() };
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = searchPaths; info.PathListInfo.Length = m_model == Model::Dlaa ? 3 : 2;
    info.LoggingInfo.LoggingCallback = OnNgxLog; info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = false;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(kAppId, m_dataPath.c_str(), m_dev, &info, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) { Fail("NGX core Init: %s", NgxResultName(r)); return false; }
    nr::ngxusers::Joined(); m_ngxJoined = true;
    NVSDK_NGX_Parameter* caps = nullptr;
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
    if (NVSDK_NGX_FAILED(r) || !caps) { Fail("GetCapabilityParameters: %s", NgxResultName(r)); return false; }
    m_caps = caps;
    return true;
}

// The model's functions: Neural Rendering through the forwarder and the person's own model file, or DLAA through NGX and NVIDIA's runtime.
bool NrEngine::StartModel() {
    if (m_model == Model::NeuralRendering) return StartForwarder();
    dlaa::SetPreset(m_dlaaPreset);
    m_create = dlaa::Create; m_evaluate = dlaa::Evaluate; m_release = dlaa::Release; m_lastResult = dlaa::LastResult;
    return true;
}

bool NrEngine::StartForwarder() {
    m_forwarder = LoadLibraryW(m_forwarderPath.c_str());
    if (!m_forwarder) { Fail("forwarder LoadLibrary %lu (%ls)", GetLastError(), m_forwarderPath.c_str()); return false; }
    struct Export { const char* name; void** fn; };
    const Export exports[] = {
        { "nrfwd_probe", (void**)&m_probe }, { "nrfwd_init", (void**)&m_init }, { "nrfwd_set_float_slot", (void**)&m_setFloatSlot },
        { "nrfwd_probe_float", (void**)&m_probeFloat }, { "nrfwd_get_float", (void**)&m_getFloat }, { "nrfwd_create", (void**)&m_create },
        { "nrfwd_evaluate", (void**)&m_evaluate }, { "nrfwd_release", (void**)&m_release }, { "nrfwd_last_result", (void**)&m_lastResult },
    };
    for (const Export& e : exports)
        if (!(*e.fn = reinterpret_cast<void*>(GetProcAddress(m_forwarder, e.name)))) { Fail("forwarder export %s missing", e.name); return false; }
    const int found = m_probe(m_snippetPath.c_str());
    if ((found & 0xF) != 0xF) { Fail("snippet probe 0x%x (%ls)", found, m_snippetPath.c_str()); return false; }
    if (!FindFloatSlot()) return false;
    const int r = m_init(m_snippetPath.c_str(), m_dataPath.c_str(), m_dev, m_caps);
    if (r != 1) { Fail("snippet Init_Ext: %s", NgxResultName(r)); return false; }
    return true;
}

// The model reads floats through getter slot 14: the right setter is the one whose value comes back there.
bool NrEngine::FindFloatSlot() {
    for (const int slot : { 6, 5, 1, 2, 4, 7 }) {
        m_probeFloat(m_caps, "NR.Probe", 1.5f, slot);
        alignas(8) uint8_t bytes[8] = {};
        const int got = m_getFloat(m_caps, "NR.Probe", bytes, kFloatGetterSlot);
        m_probeFloat(m_caps, "NR.Probe", 0.0f, slot);
        float value; memcpy(&value, bytes, sizeof value);
        if (got == 1 && std::fabs(value - 1.5f) < 1e-6f) { m_setFloatSlot(slot); m_stats.floatSlot = slot; return true; }
    }
    Fail("no float setter slot round-trips through getter 14");
    return false;
}

// One root signature for the passes of our own: a table t0..t3, a table u0..u1, twelve root constants (b0) and a linear clamp sampler (s0).
bool NrEngine::CreatePipelines() {
    D3D12_DESCRIPTOR_RANGE srvs{}; srvs.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srvs.NumDescriptors = 4;
    D3D12_DESCRIPTOR_RANGE uavs{}; uavs.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uavs.NumDescriptors = 2;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable = { 1, &srvs };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[1].DescriptorTable = { 1, &uavs };
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[2].Constants.Num32BitValues = sizeof(PassConstants) / 4;
    for (D3D12_ROOT_PARAMETER& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    D3D12_ROOT_SIGNATURE_DESC desc{ 3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = nullptr, * error = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error))) {
        Fail("root signature: %s", error ? static_cast<const char*>(error->GetBufferPointer()) : "?"); SafeRelease(error); return false;
    }
    HRESULT hr = m_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig));
    blob->Release();
    if (FAILED(hr)) { Fail("CreateRootSignature 0x%08x", (unsigned)hr); return false; }

    auto build = [&](const char* source, const char* entry, ID3D12PipelineState** pso) {
        ID3DBlob* code = nullptr, * err = nullptr;
        if (FAILED(D3DCompile(source, strlen(source), "nr_model", nullptr, nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
            Fail("HLSL %s: %s", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); SafeRelease(err); return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = m_rootSig; pd.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        const HRESULT h = m_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(pso));
        code->Release();
        if (FAILED(h)) { Fail("PSO %s 0x%08x", entry, (unsigned)h); return false; }
        return true;
    };
    if (!build(kNrModelHlsl, "CSDown", &m_psoShrink) || !build(kNrModelHlsl, "CSFlowToMvec", &m_psoMotion) ||
        !build(kNrModelHlsl, "CSDelta", &m_psoDelta) || !build(kNrSmoothHlsl, "CSDeltaSmooth", &m_psoDeltaSmooth)) return false;
    static_assert(FlowEstimator::kSlots == kSlots, "the estimator reads its statistics back per engine slot");
    if (m_estimator.Init(m_dev, [this](const char* m) { Log("%s", m); })) m_estimator.SetTimestampFrequency(m_timestampFreq);
    else Log("NrEngine: the motion estimator could not start; the model gets LSFG's flow");

    // a block of descriptors per allocator slot, so a list still on the GPU never sees its descriptors rewritten
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = kDescriptorsPerSlot * kSlots; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_dev->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_heap)))) { Fail("CreateDescriptorHeap"); return false; }
    m_descriptorSize = m_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------------------------------------------

ID3D12Resource* NrEngine::MakeTexture(uint32_t w, uint32_t h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = w; desc.Height = h; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = fmt; desc.SampleDesc.Count = 1; desc.Flags = flags;
    ID3D12Resource* texture = nullptr;
    const HRESULT hr = m_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&texture));
    if (FAILED(hr)) Log("a %ux%u texture (format %d) could not be made: 0x%08x", w, h, (int)fmt, (unsigned)hr);
    return texture;
}

void NrEngine::Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) { Transition(m_list, r, from, to); }
void NrEngine::Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to };
    list->ResourceBarrier(1, &b);
}

// Records the copy on `list` and leaves the texture readable; the upload buffer goes into `staging`, to be released once the GPU has read it.
void NrEngine::Upload(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, uint32_t bytesPerPixel, const void* pixels, std::vector<ID3D12Resource*>& staging) {
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; UINT rows = 0; UINT64 rowBytes = 0, total = 0;
    m_dev->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &rows, &rowBytes, &total);
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = total; buffer.Height = 1;
    buffer.DepthOrArraySize = 1; buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* upload = nullptr;
    if (FAILED(m_dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) return;
    uint8_t* mapped = nullptr;
    upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    const size_t srcPitch = static_cast<size_t>(desc.Width) * bytesPerPixel;
    for (UINT y = 0; y < rows; ++y) memcpy(mapped + layout.Offset + y * layout.Footprint.RowPitch, static_cast<const uint8_t*>(pixels) + y * srcPitch, srcPitch);
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource = texture; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = upload; from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = layout;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Transition(list, texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    staging.push_back(upload);
}

void NrEngine::Drain() {
    if (!m_dev) return;
    if (!WaitWorkerIdle(kIdleWaitMs)) { Log("NrEngine: the engine's thread is still busy; not waiting for it"); return; }
    WaitIdle();
}

void NrEngine::StartWorker() {
    if (m_worker.joinable()) return;
    { std::lock_guard<std::mutex> lock(m_jobMutex); m_stop = false; m_busy = false; m_jobs.clear(); }
    m_submitted = 0; m_busySince = 0; m_stuck = false;
    for (auto& v : m_okRing) v = 0;
    if (!m_workerExited) m_workerExited = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(m_workerExited);
    m_worker = std::thread([this] { WorkerLoop(); });
}

bool NrEngine::StopWorker(DWORD ms) {
    if (!m_worker.joinable()) return true;
    { std::lock_guard<std::mutex> lock(m_jobMutex); m_stop = true; }
    m_jobCv.notify_all();
    if (WaitForSingleObject(m_workerExited, ms) != WAIT_OBJECT_0) { m_worker.detach(); return false; }
    m_worker.join();
    return true;
}

bool NrEngine::WaitWorkerIdle(DWORD ms) {
    if (m_stuck) return false;
    std::unique_lock<std::mutex> lock(m_jobMutex);
    return m_jobCv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return m_jobs.empty() && !m_busy; });
}

bool NrEngine::Busy() {
    std::lock_guard<std::mutex> lock(m_jobMutex);
    return m_busy || !m_jobs.empty();
}

bool NrEngine::WaitSubmitted(uint64_t signalValue, DWORD ms) {
    std::unique_lock<std::mutex> lock(m_jobMutex);
    return m_jobCv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return m_submitted.load() >= signalValue || m_stuck.load(); }) && !m_stuck;
}

// "finished" for a run that queued nothing: on the queue after the runs before it (or from the CPU when there is no queue to order it on)
static void SignalUnqueued(ID3D12CommandQueue* queue, const NrEngine::Job& j) {
    if (!j.signalFence) return;
    if (queue) queue->Signal(j.signalFence, j.signalValue); else j.signalFence->Signal(j.signalValue);
}

void NrEngine::Submit(const Job& j) {
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        if (m_worker.joinable() && !m_stop && !m_stuck) { m_jobs.push_back(j); m_jobCv.notify_all(); return; }
    }
    if (!m_stuck) SignalUnqueued(m_queue, j);   // no thread to run it: "finished" still moves on (nothing ran)
}

void NrEngine::WorkerLoop() {
    // on the frame path now (a frame waits for this thread to record it), in short bursts: ahead of a busy game's threads, so the scheduler
    // does not hold it up by milliseconds when every core is loaded
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadDescription(GetCurrentThread(), L"LS Addon engine");
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobCv.wait(lock, [&] { return m_stop || !m_jobs.empty(); });
            if (m_stop) {   // the runs left are marked finished, so nothing waits for them
                for (const Job& left : m_jobs) SignalUnqueued(m_queue, left);
                m_jobs.clear();
                break;
            }
            j = m_jobs.front(); m_jobs.pop_front(); m_busy = true;
        }
        m_busySince = GetTickCount64();
        // marked as going through before it is submitted (the GPU may finish it before this thread gets back), taken back if it did not
        m_okRing[j.signalValue % kOkRing].store(j.signalValue, std::memory_order_release);
        const uint64_t queuedBefore = m_stats.frames;
        const bool ok = Run(j.sharedIn, j.sharedDelta, j.waitFence, j.waitValue, j.usedFence, j.usedValue, j.signalFence, j.signalValue, j.reset, j.sharedMotion);
        const bool queued = m_stats.frames != queuedBefore;
        if (!queued) SignalUnqueued(m_queue, j);   // (Run queues nothing while a new model is made, or when the GPU is far behind)
        m_busySince = 0;
        m_okRing[j.signalValue % kOkRing].store(ok && queued ? j.signalValue : 0, std::memory_order_release);
        m_submitted.store(j.signalValue, std::memory_order_release);
        { std::lock_guard<std::mutex> lock(m_jobMutex); m_busy = false; }
        m_jobCv.notify_all();
    }
    SetEvent(m_workerExited);
}

bool NrEngine::CheckStuck() {
    if (m_stuck) return true;
    const ULONGLONG since = m_busySince.load();
    if (!since || GetTickCount64() - since < kStuckMs) return false;
    m_stuck = true;
    Fail("the model stopped responding (a run has been in NVIDIA's code for %llu ms); Lossless Scaling runs untouched. Restart Lossless Scaling to use it again",
         (unsigned long long)(GetTickCount64() - since));
    m_jobCv.notify_all();
    return true;
}

bool NrEngine::WaitIdle() {
    if (!m_queue || !m_fence) return true;
    m_queue->Signal(m_fence, ++m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue) { m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent); WaitForSingleObject(m_fenceEvent, kIdleWaitMs); }
    if (m_fence->GetCompletedValue() < m_fenceValue) { Log("NrEngine: the GPU did not finish within %lu ms", kIdleWaitMs); return false; }
    return true;
}

int NrEngine::TakeSlot() {
    const int slot = m_nextSlot;
    if (m_slotDone[slot] && m_fence->GetCompletedValue() < m_slotDone[slot]) {
        m_fence->SetEventOnCompletion(m_slotDone[slot], m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, kSlotWaitMs);
        // still busy: resetting the allocator now would pull its commands from under the GPU, so this frame is not run
        if (m_fence->GetCompletedValue() < m_slotDone[slot]) return -1;
    }
    m_nextSlot = (m_nextSlot + 1) % kSlots;
    m_alloc[slot]->Reset();
    return slot;
}

// The times of this slot's previous run (finished: TakeSlot waited for it), and where it sat on the CPU's clock relative to its submission.
void NrEngine::ReadTimes(int slot) {
    if (!m_slotDone[slot]) return;
    const D3D12_RANGE range{ static_cast<SIZE_T>(slot) * 32, static_cast<SIZE_T>(slot) * 32 + 32 };
    uint64_t* mapped = nullptr;
    if (FAILED(m_timestampReadback->Map(0, &range, reinterpret_cast<void**>(&mapped)))) return;
    const uint64_t* t = mapped + slot * 4;
    const double msPerTick = 1000.0 / static_cast<double>(m_timestampFreq);
    m_stats.nrMs = static_cast<double>(t[2] - t[1]) * msPerTick;
    m_stats.totalMs = static_cast<double>(t[3] - t[0]) * msPerTick;
    uint64_t gpuNow = 0, cpuNow = 0; LARGE_INTEGER qpcFreq; QueryPerformanceFrequency(&qpcFreq);
    if (m_slotSubmitQpc[slot] && SUCCEEDED(m_queue->GetClockCalibration(&gpuNow, &cpuNow))) {
        const double sinceSubmitMs = static_cast<double>(static_cast<int64_t>(cpuNow) - m_slotSubmitQpc[slot]) * 1000.0 / static_cast<double>(qpcFreq.QuadPart);
        auto onCpuClock = [&](uint64_t gpu) { return sinceSubmitMs + (static_cast<double>(gpu) - static_cast<double>(gpuNow)) * msPerTick; };
        m_stats.startMs = onCpuClock(t[0]); m_stats.doneMs = onCpuClock(t[3]);
    }
    const D3D12_RANGE nothingWritten{ 0, 0 };
    m_timestampReadback->Unmap(0, &nothingWritten);
}

D3D12_CPU_DESCRIPTOR_HANDLE NrEngine::CpuDescriptor(int slot, int pass, int i) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot * kDescriptorsPerSlot + pass * kPassDescriptors + i) * m_descriptorSize;
    return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE NrEngine::GpuDescriptor(int slot, int pass, int i) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = m_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(slot * kDescriptorsPerSlot + pass * kPassDescriptors + i) * m_descriptorSize;
    return h;
}

ID3D12Resource* NrEngine::OpenSharedTexture(HANDLE h) {
    ID3D12Resource* r = nullptr;
    const HRESULT hr = m_dev->OpenSharedHandle(h, IID_PPV_ARGS(&r));
    if (FAILED(hr)) Log("NrEngine: a shared texture could not be opened: 0x%08x", (unsigned)hr);
    return r;
}
ID3D12Fence* NrEngine::OpenSharedFence(HANDLE h) {
    ID3D12Fence* f = nullptr;
    const HRESULT hr = m_dev->OpenSharedHandle(h, IID_PPV_ARGS(&f));
    if (FAILED(hr)) Log("NrEngine: a shared fence could not be opened: 0x%08x", (unsigned)hr);
    return f;
}

void NrEngine::SetFlowInput(ID3D12Resource* flow, uint32_t w, uint32_t h) {
    const bool changed = (flow != nullptr) != (m_flow != nullptr) || (flow && (w != m_flowW || h != m_flowH));
    if (changed) { if (flow) Log("flow input: LSFG flow %ux%u -> model motion vectors", w, h); else Log("flow input: none (zero motion vectors)"); }
    m_flow = flow; m_flowW = flow ? w : 0; m_flowH = flow ? h : 0;
    m_stats.hasFlow = flow != nullptr; m_stats.flowW = m_flowW; m_stats.flowH = m_flowH;
}

void NrEngine::ReleaseScratch() {
    if (m_feature) { WaitIdle(); std::lock_guard<std::mutex> lock(m_ngxMutex); m_release(m_feature); m_feature = nullptr; }
    for (ID3D12Resource** t : { &m_proxy, &m_out[0], &m_out[1], &m_depth, &m_mvec, &m_history[0], &m_history[1] }) SafeRelease(*t);
    m_historyValid = false; m_historyRead = 0;
    m_w = m_h = m_ww = m_wh = 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Prepare: the feature and the scratch textures
// ---------------------------------------------------------------------------------------------------------------------------------

bool NrEngine::Prepare(uint32_t w, uint32_t h, DXGI_FORMAT fmt, const NrParams& p) {
    if (!m_ready) return false;
    // the working size: the frame scaled by 0.25..1, rounded up to a multiple of 8, at least 64 (or the frame) and at most the frame
    const float scale = m_model == Model::Dlaa ? 1.0f : std::clamp(p.workingScale, 0.25f, 1.0f);   // DLAA works on the whole frame
    auto workSize = [&](uint32_t full) {
        uint32_t s = (static_cast<uint32_t>(full * scale) + 7) & ~7u;
        s = std::min(s, full);
        return std::max(s, std::min(full, 64u));
    };
    const uint32_t ww = workSize(w), wh = workSize(h);
    // the frame's size and format are the runs' own: the shrink pass takes any frame to the working size there is
    m_w = w; m_h = h; m_fmt = fmt; m_params = p;

    const int state = m_buildState.load(std::memory_order_acquire);
    if (state == kBuildFailed) { EndBuild(); m_buildState = kIdle; Fail("%s", m_buildError); return false; }
    if (state == kBuilt) {
        EndBuild();
        if (m_built.ww == ww && m_built.wh == wh) TakeBuilt();
        else { Log("NrEngine: a model for %ux%u is ready but %ux%u is wanted now: it is dropped", m_built.ww, m_built.wh, ww, wh); Discard(m_built); }
        m_buildState = kIdle;
    }
    if (m_feature && ww == m_ww && wh == m_wh) return true;   // the rest applies on the next run
    if (m_buildState.load() == kIdle && !StartBuild(ww, wh, p.Tuning())) return false;
    return m_feature != nullptr;   // the set there is keeps running until the new one is ready
}

bool NrEngine::StartBuild(uint32_t ww, uint32_t wh, const NrTuning& tuning) {
    if (!m_buildQueue) {
        D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(m_dev->CreateCommandQueue(&queue, IID_PPV_ARGS(&m_buildQueue))) ||
            FAILED(m_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_buildAlloc))) ||
            FAILED(m_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_buildAlloc, nullptr, IID_PPV_ARGS(&m_buildList))) ||
            FAILED(m_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_buildFence))) || !(m_buildEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr))) {
            Fail("the queue that makes the model could not be made"); return false;
        }
        m_buildList->Close();
    }
    Log("Prepare: frame %ux%u (%s) -> model input %ux%u (%.2f MP), style %u intensity %.2f: made on a thread of its own", m_w, m_h,
        m_fmt == DXGI_FORMAT_B8G8R8A8_UNORM ? "BGRA8" : m_fmt == DXGI_FORMAT_R8G8B8A8_UNORM ? "RGBA8" : m_fmt == DXGI_FORMAT_R10G10B10A2_UNORM ? "RGB10A2"
        : m_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? "RGBA16F" : "fmt?", ww, wh, ww * wh / 1e6, tuning.style, tuning.intensity);
    m_buildW = ww; m_buildH = wh; m_buildTuning = tuning; m_buildError[0] = 0;
    m_buildState = kBuilding;
    m_buildThread = CreateThread(nullptr, 0, BuildThread, this, 0, nullptr);
    if (!m_buildThread) { m_buildState = kIdle; Fail("the thread that makes the model could not be started"); return false; }
    return true;
}

DWORD WINAPI NrEngine::BuildThread(void* self) { static_cast<NrEngine*>(self)->Build(); return 0; }

void NrEngine::Build() {
    LARGE_INTEGER qf, q0, q1, q2, q3; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    auto ms = [&](const LARGE_INTEGER& a, const LARGE_INTEGER& b) { return (b.QuadPart - a.QuadPart) * 1000.0 / qf.QuadPart; };
    auto fail = [&](Scratch& s, const char* why) {
        snprintf(m_buildError, sizeof m_buildError, "%s", why);
        Discard(s);
        m_buildState.store(kBuildFailed, std::memory_order_release);
    };
    Scratch s; s.ww = m_buildW; s.wh = m_buildH;
    const uint32_t ww = s.ww, wh = s.wh;
    const auto writable = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    s.proxy = MakeTexture(ww, wh, DXGI_FORMAT_R8G8B8A8_UNORM, writable, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (ID3D12Resource*& out : s.out) out = MakeTexture(ww, wh, DXGI_FORMAT_R8G8B8A8_UNORM, writable, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (ID3D12Resource*& hist : s.history) hist = MakeTexture(ww, wh, DXGI_FORMAT_R16G16B16A16_FLOAT, writable, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    s.depth = MakeTexture(ww, wh, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    s.mvec = MakeTexture(ww, wh, DXGI_FORMAT_R16G16_FLOAT, writable, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!s.proxy || !s.out[0] || !s.out[1] || !s.history[0] || !s.history[1] || !s.depth || !s.mvec) { fail(s, "scratch allocation"); return; }
    QueryPerformanceCounter(&q1);

    // flat depth (0.5; the model ignores it) and zero motion, uploaded once; then the feature, which records its own setup on the same list
    std::vector<ID3D12Resource*> staging;
    m_buildAlloc->Reset();
    m_buildList->Reset(m_buildAlloc, nullptr);
    const std::vector<float> depth(static_cast<size_t>(ww) * wh, 0.5f);
    const std::vector<uint16_t> motion(static_cast<size_t>(ww) * wh * 2, 0);
    Upload(m_buildList, s.depth, 4, depth.data(), staging);
    Upload(m_buildList, s.mvec, 4, motion.data(), staging);
    NrCreateParams create{}; create.width = ww; create.height = wh; create.preset = 0; create.scalingRatio = 1.0f; create.tuning = m_buildTuning;
    char why[128] = {};
    {
        std::lock_guard<std::mutex> lock(m_ngxMutex);
        s.feature = m_create(m_buildList, m_caps, &create);
        if (!s.feature) snprintf(why, sizeof why, "CreateFeature(18): %s", NgxResultName(m_lastResult(1)));
    }
    QueryPerformanceCounter(&q2);
    m_buildList->Close();
    ID3D12CommandList* lists[] = { m_buildList };
    m_buildQueue->ExecuteCommandLists(1, lists);
    m_buildQueue->Signal(m_buildFence, ++m_buildFenceValue);
    if (m_buildFence->GetCompletedValue() < m_buildFenceValue) { m_buildFence->SetEventOnCompletion(m_buildFenceValue, m_buildEvent); WaitForSingleObject(m_buildEvent, kIdleWaitMs); }
    if (m_buildFence->GetCompletedValue() < m_buildFenceValue) {   // the GPU may still use all of it: leak rather than crash
        snprintf(m_buildError, sizeof m_buildError, "the GPU did not finish setting up the model");
        m_buildState.store(kBuildFailed, std::memory_order_release);
        return;
    }
    for (ID3D12Resource* r : staging) r->Release();
    QueryPerformanceCounter(&q3);
    if (!s.feature) { fail(s, why); return; }
    m_stats.lastBuildMs = ms(q0, q3);
    Log("NrEngine: model for %ux%u made in %.1f ms off the frame path: textures %.1f, feature (CPU) %.1f, GPU setup %.1f", ww, wh, ms(q0, q3), ms(q0, q1), ms(q1, q2), ms(q2, q3));
    m_built = s;
    m_buildState.store(kBuilt, std::memory_order_release);
}

void NrEngine::EndBuild() {
    if (!m_buildThread) return;
    WaitForSingleObject(m_buildThread, INFINITE);   // it has ended, or ends within the GPU wait's limit
    CloseHandle(m_buildThread);
    m_buildThread = nullptr;
}

void NrEngine::Discard(Scratch& s) {
    if (s.feature) { std::lock_guard<std::mutex> lock(m_ngxMutex); m_release(s.feature); }
    for (ID3D12Resource** t : { &s.proxy, &s.out[0], &s.out[1], &s.depth, &s.mvec, &s.history[0], &s.history[1] }) SafeRelease(*t);
    s = Scratch{};
}

void NrEngine::TakeBuilt() {
    if (m_feature) {   // the runs still queued use the old set: it goes once they are done (at most the one in flight)
        if (!WaitIdle()) { Log("NrEngine: the GPU did not finish the last run: the old model is kept"); Discard(m_built); return; }
        Scratch old; old.feature = m_feature; old.proxy = m_proxy; old.out[0] = m_out[0]; old.out[1] = m_out[1];
        old.history[0] = m_history[0]; old.history[1] = m_history[1]; old.depth = m_depth; old.mvec = m_mvec;
        Discard(old);
    }
    m_feature = m_built.feature; m_proxy = m_built.proxy; m_out[0] = m_built.out[0]; m_out[1] = m_built.out[1];
    m_history[0] = m_built.history[0]; m_history[1] = m_built.history[1]; m_depth = m_built.depth; m_mvec = m_built.mvec;
    m_ww = m_built.ww; m_wh = m_built.wh;
    m_built = Scratch{};
    m_stats.workW = m_ww; m_stats.workH = m_wh; ++m_stats.builds;
    m_historyValid = false; m_historyRead = 0; m_resetHistory = true;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// One run
// ---------------------------------------------------------------------------------------------------------------------------------
//
// The descriptors of each pass, per allocator slot (t0..t3, then u0, u1; null where the pass reads or writes nothing):
//   shrink   t0 the frame                                                   u0 the proxy
//   motion   t3 LSFG's flow (null without)                                  u1 the motion vectors
//   delta    t0 the last smoothed delta (smoothing only), t1 the proxy,     u0 the shared delta, u1 the new smoothed delta (smoothing only)
//            t2 the model's last output, t3 the motion vectors

bool NrEngine::Run(ID3D12Resource* sharedIn, ID3D12Resource* sharedDelta, ID3D12Fence* waitFence, uint64_t waitValue, ID3D12Fence* usedFence,
                   uint64_t usedValue, ID3D12Fence* signalFence, uint64_t signalValue, bool reset, ID3D12Resource* sharedMotion) {
    if (!m_ready || !m_feature) return false;
    // a new feature is being made: the model cannot run meanwhile, and the frame path does not wait for it (the last result carries on)
    std::unique_lock<std::mutex> ngx(m_ngxMutex, std::try_to_lock);
    if (!ngx.owns_lock()) { ++m_stats.busySkips; return false; }
    const int slot = TakeSlot();
    if (slot < 0) { Log("NrEngine: the GPU is still busy with a run from %d frames ago: this frame is skipped", kSlots); return false; }
    ReadTimes(slot);

    m_estimator.Collect(m_fence->GetCompletedValue());   // textures of an earlier working size, once no list uses them
    m_estimator.ReadStats(slot);
    const int passes = m_model == Model::Dlaa ? 1 : std::clamp(static_cast<int>(m_params.passes), 1, 4);
    const bool smooth = m_params.deltaSmooth > 0.001f;
    const bool historyUsable = m_historyValid && !reset && !m_resetHistory;
    ID3D12Resource* const lastOut = m_out[(passes - 1) % 2];
    ID3D12Resource* const historyIn = m_history[m_historyRead];
    ID3D12Resource* const historyOut = m_history[1 - m_historyRead];

    // descriptors
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    auto readable = [&](int pass, int t, ID3D12Resource* r, DXGI_FORMAT f) { srv.Format = f; m_dev->CreateShaderResourceView(r, &srv, CpuDescriptor(slot, pass, t)); };
    auto writable = [&](int pass, int u, ID3D12Resource* r, DXGI_FORMAT f) { uav.Format = f; m_dev->CreateUnorderedAccessView(r, nullptr, &uav, CpuDescriptor(slot, pass, 4 + u)); };
    const DXGI_FORMAT rgba8 = DXGI_FORMAT_R8G8B8A8_UNORM, rgba16f = DXGI_FORMAT_R16G16B16A16_FLOAT, rg16f = DXGI_FORMAT_R16G16_FLOAT;
    readable(kShrink, 0, sharedIn, m_fmt); readable(kShrink, 1, nullptr, rgba8); readable(kShrink, 2, nullptr, rgba8); readable(kShrink, 3, nullptr, rgba16f);
    writable(kShrink, 0, m_proxy, rgba8); writable(kShrink, 1, nullptr, rg16f);
    readable(kMotion, 0, nullptr, rgba8); readable(kMotion, 1, nullptr, rgba8); readable(kMotion, 2, nullptr, rgba8); readable(kMotion, 3, m_flow, rgba16f);
    writable(kMotion, 0, nullptr, rgba16f); writable(kMotion, 1, m_mvec, rg16f);
    readable(kDeltaPass, 0, smooth ? historyIn : nullptr, rgba16f); readable(kDeltaPass, 1, m_proxy, rgba8); readable(kDeltaPass, 2, lastOut, rgba8);
    readable(kDeltaPass, 3, m_mvec, rg16f);
    writable(kDeltaPass, 0, sharedDelta, rgba16f); writable(kDeltaPass, 1, smooth ? historyOut : nullptr, rgba16f);

    m_list->Reset(m_alloc[slot], nullptr);
    const UINT q = slot * 4;
    m_list->EndQuery(m_timestamps, D3D12_QUERY_TYPE_TIMESTAMP, q);
    ID3D12DescriptorHeap* heaps[] = { m_heap };
    auto bindOurs = [&] { m_list->SetDescriptorHeaps(1, heaps); m_list->SetComputeRootSignature(m_rootSig); };
    auto dispatch = [&](ID3D12PipelineState* pso, int pass, const PassConstants& c) {
        m_list->SetPipelineState(pso);
        m_list->SetComputeRootDescriptorTable(0, GpuDescriptor(slot, pass, 0));
        m_list->SetComputeRootDescriptorTable(1, GpuDescriptor(slot, pass, 4));
        m_list->SetComputeRoot32BitConstants(2, sizeof c / 4, &c, 0);
        m_list->Dispatch((c.dstW + 7) / 8, (c.dstH + 7) / 8, 1);
    };
    bindOurs();

    // the shared textures come in COMMON
    Transition(sharedIn, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(sharedDelta, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // 1. the frame, shrunk to the proxy
    Transition(m_proxy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    { PassConstants shrink{ m_ww, m_wh, m_w, m_h }; shrink.encoding = m_encoding; shrink.white = m_whiteNits; dispatch(m_psoShrink, kShrink, shrink); }
    Transition(m_proxy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // 2. the model's motion vectors, in working-size pixels. Measured from the proxy itself (the default: per pixel, this frame's own, the
    //    picture the model sees), with its textures following a new working size without a wait (the old ones retire once the GPU is past
    //    them). Or LSFG's flow (below). The compose keeps using LSFG's flow for the generated frames either way.
    const bool measure = m_params.useFlow && m_params.modelMotion == 0 && m_estimator.IsReady() &&
                         (!m_estimator.NeedsResize(m_ww, m_wh) || m_estimator.Ensure(m_ww, m_wh, m_fenceValue));
    if (measure) {
        if (!m_estimatedLast) m_estimator.Forget();   // its frame before is not the one before this
        Transition(m_mvec, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_estimator.Record(m_list, slot, m_proxy, DXGI_FORMAT_R8G8B8A8_UNORM, m_mvec, nullptr);
        Transition(m_mvec, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        bindOurs();   // the estimator binds its own heap and root signature
    }
    m_estimatedLast = measure;
    // LSFG's flow as motion vectors in working-size pixels (zero without flow). One flow unit is W / (flowUnit * flow width) frame pixels
    // (measured: units of a texture flowUnit times the flow's size), and a frame pixel is ww / W working pixels.
    if (!measure) {
        const bool withFlow = m_flow && m_params.useFlow && m_flowW && m_flowH;
        const float unit = m_params.flowUnit > 0.1f ? m_params.flowUnit : 2.0f;
        PassConstants c{ m_ww, m_wh, m_flowW, m_flowH, withFlow ? 1u : 0u };
        c.flowScale = withFlow ? static_cast<float>(m_ww) / (unit * static_cast<float>(m_flowW)) : 0.0f;
        Transition(m_mvec, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(m_psoMotion, kMotion, c);
        Transition(m_mvec, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    // the motion goes with the result (the texture comes in COMMON, like the delta); a size that does not match is left out, never copied
    if (sharedMotion) {
        const D3D12_RESOURCE_DESC to = sharedMotion->GetDesc();
        if (to.Width == m_ww && to.Height == m_wh && to.Format == DXGI_FORMAT_R16G16_FLOAT) {
            Transition(m_mvec, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(sharedMotion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            m_list->CopyResource(sharedMotion, m_mvec);
            Transition(sharedMotion, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            Transition(m_mvec, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }

    // 3. the model: the proxy (with the motion vectors and flat depth) into m_out[0]; each further pass reads the last result and writes the
    //    other buffer. Both rest writable; the one a pass reads is made readable first.
    m_list->EndQuery(m_timestamps, D3D12_QUERY_TYPE_TIMESTAMP, q + 1);
    NrEvalParams e{};
    e.depth = m_depth; e.mvec = m_mvec; e.width = e.guideWidth = m_ww; e.height = e.guideHeight = m_wh;
    e.mvScaleX = e.mvScaleY = 1.0f; e.scalingRatio = 1.0f; e.tuning = m_params.Tuning();
    D3D12_RESOURCE_STATES outState[2] = { D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    auto setOutState = [&](int i, D3D12_RESOURCE_STATES want) { if (outState[i] != want) { Transition(m_out[i], outState[i], want); outState[i] = want; } };
    int evalResult = 1;
    for (int k = 0; k < passes; ++k) {
        const int dst = k % 2;
        if (k > 0) setOutState(1 - dst, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        setOutState(dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        e.color = k == 0 ? m_proxy : m_out[1 - dst];
        e.output = m_out[dst];
        e.reset = k == 0 && (reset || m_resetHistory) ? 1u : 0u;   // the model's history starts afresh once, not at every pass
        const int r = m_evaluate(m_list, m_feature, m_caps, &e);
        if (r != 1 && evalResult == 1) evalResult = r;
    }
    m_resetHistory = false;
    m_list->EndQuery(m_timestamps, D3D12_QUERY_TYPE_TIMESTAMP, q + 2);
    bindOurs();   // the model leaves its own heap and root signature bound
    setOutState((passes - 1) % 2, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // 4. the delta (model - proxy) into the shared texture; with smoothing, blended with the last one moved along the motion
    PassConstants c{ m_ww, m_wh, m_ww, m_wh };
    if (smooth) {
        c.flags = historyUsable ? 2u : 0u;
        c.smoothAmount = std::clamp(m_params.deltaSmooth, 0.0f, 0.95f);
        c.flowScale = 0.005f * static_cast<float>(m_ww);   // fast motion (working-size pixels a frame): the smoothing fades out (see CSDeltaSmooth)
        Transition(historyOut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch(m_psoDeltaSmooth, kDeltaPass, c);
        Transition(historyOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_historyRead = 1 - m_historyRead; m_historyValid = true;
    } else {
        dispatch(m_psoDelta, kDeltaPass, c);
        m_historyValid = false;   // no history is kept while smoothing is off
    }

    setOutState(0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); setOutState(1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(sharedIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Transition(sharedDelta, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    m_list->EndQuery(m_timestamps, D3D12_QUERY_TYPE_TIMESTAMP, q + 3);
    m_list->ResolveQueryData(m_timestamps, D3D12_QUERY_TYPE_TIMESTAMP, q, 4, m_timestampReadback, static_cast<UINT64>(slot) * 32);
    m_list->Close();

    if (evalResult != 1) { ++m_stats.fails; snprintf(m_stats.lastError, sizeof m_stats.lastError, "evaluate: %s", NgxResultName(evalResult)); }
    m_queue->Wait(waitFence, waitValue);
    if (usedFence) m_queue->Wait(usedFence, usedValue);
    ID3D12CommandList* lists[] = { m_list };
    m_queue->ExecuteCommandLists(1, lists);
    m_queue->Signal(signalFence, signalValue);
    m_queue->Signal(m_fence, ++m_fenceValue);
    m_slotDone[slot] = m_fenceValue;
    LARGE_INTEGER now; QueryPerformanceCounter(&now); m_slotSubmitQpc[slot] = now.QuadPart;
    ++m_stats.frames;
    if (measure && (++m_estimates == 60 || m_estimates % 1200 == 0)) {   // what the estimate found (a check that it follows the picture)
        double x = 0, y = 0, length = 0, cost = 0, distrust = 0; uint64_t frames = 0; double stage[4];
        if (m_estimator.TakeAverages(x, y, length, cost, distrust, frames))
            Log("motion estimator (the model's motion, %ux%u): over %llu frames, average vector (%.2f, %.2f) px, average length %.2f px, match cost %.4f",
                m_ww, m_wh, (unsigned long long)frames, x, y, length, cost);
        if (m_estimator.TakeStageTimes(stage))
            Log("motion estimator: stages %.3f ms pyramid, %.3f search, %.3f median, %.3f every pixel", stage[0], stage[1], stage[2], stage[3]);
    }
    return evalResult == 1;
}
