#include "addon/bridge.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace {
template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

double Smooth(double avg, double sample) { return avg == 0 ? sample : avg * 0.9 + sample * 0.1; }
double Ms(int64_t ticks, int64_t freq) { return (double)ticks * 1000.0 / (double)freq; }
int64_t Now() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }

// Keys for LogOnce: what failed, and for which size and format.
uint64_t FailureKey(char what, uint32_t w, uint32_t h, uint32_t fmt) { return ((uint64_t)(uint8_t)what << 56) ^ ((uint64_t)w << 36) ^ ((uint64_t)h << 16) ^ fmt; }
}

void Bridge::SharedTexture::Release() { SafeRelease(view); SafeRelease(d3d12); SafeRelease(d3d11); w = h = 0; }
void Bridge::SharedFence::Release() { SafeRelease(d3d12); SafeRelease(d3d11); }

void Bridge::Log(const char* fmt, ...) {
    if (!m_log) return;
    char text[512]; va_list args; va_start(args, fmt); vsnprintf(text, sizeof text, fmt, args); va_end(args);
    m_log(text);
}
void Bridge::LogOnce(uint64_t key, const char* fmt, ...) {
    if (key == m_lastFailure || !m_log) return;
    m_lastFailure = key;
    char text[512]; va_list args; va_start(args, fmt); vsnprintf(text, sizeof text, fmt, args); va_end(args);
    m_log(text);
}

DXGI_FORMAT Bridge::ViewFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
// 8-bit frames, and 10-bit and half-float ones (HDR, or 10-bit SDR): the engine's shrink pass gives the model their SDR view (hdr_hlsl.h).
bool Bridge::FormatSupported(DXGI_FORMAT f) {
    const DXGI_FORMAT view = ViewFormat(f);
    return view == DXGI_FORMAT_R8G8B8A8_UNORM || view == DXGI_FORMAT_B8G8R8A8_UNORM || view == DXGI_FORMAT_R10G10B10A2_UNORM ||
           view == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool Bridge::MakeFence(SharedFence& f, const char* name) {
    if (FAILED(m_dev->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f.d3d11)))) { Log("Bridge: the %s fence could not be created", name); return false; }
    HANDLE handle = nullptr;
    if (FAILED(f.d3d11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle))) { Log("Bridge: the %s fence could not be shared", name); f.Release(); return false; }
    f.d3d12 = m_engine->OpenSharedFence(handle);
    CloseHandle(handle);
    if (!f.d3d12) { f.Release(); return false; }
    return true;
}

// Every shared texture is one mip, no multisampling, shared through an NT handle (which D3D11 wants paired with SHARED). The model only reads the
// input and flow copies; it writes the result slots, which Lossless Scaling's side then reads through a view.
bool Bridge::MakeTexture(SharedTexture& t, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool modelWrites, const char* name) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1; desc.Format = fmt; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (modelWrites ? D3D11_BIND_UNORDERED_ACCESS : 0u);
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = m_dev->CreateTexture2D(&desc, nullptr, &t.d3d11);
    if (SUCCEEDED(hr)) {
        IDXGIResource1* dxgi = nullptr;
        hr = t.d3d11->QueryInterface(IID_PPV_ARGS(&dxgi));
        HANDLE handle = nullptr;
        if (SUCCEEDED(hr)) { hr = dxgi->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle); dxgi->Release(); }
        if (SUCCEEDED(hr)) { t.d3d12 = m_engine->OpenSharedTexture(handle); CloseHandle(handle); if (!t.d3d12) hr = E_FAIL; }
    }
    if (SUCCEEDED(hr) && modelWrites) hr = m_dev->CreateShaderResourceView(t.d3d11, nullptr, &t.view);
    if (FAILED(hr)) {
        LogOnce(FailureKey(name[0], w, h, (uint32_t)fmt), "Bridge: the shared %s (%ux%u, format %d) could not be made: 0x%08x", name, w, h, (int)fmt, (unsigned)hr);
        t.Release();
        return false;
    }
    t.w = w; t.h = h;
    return true;
}

bool Bridge::Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, NrEngine* engine, LogFn log) {
    Shutdown();
    m_log = std::move(log); m_engine = engine; m_ctx = ctx;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&m_dev)))) { Log("Bridge: Lossless Scaling's device has no ID3D11Device5"); Shutdown(); return false; }
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&m_ctx4)))) { Log("Bridge: Lossless Scaling's context has no ID3D11DeviceContext4"); Shutdown(); return false; }
    if (!MakeFence(m_copied, "copied") || !MakeFence(m_finished, "finished") || !MakeFence(m_released, "released")) { Shutdown(); return false; }
    if (!m_submitTimes.Init(m_dev) || !m_composeTimes.Init(m_dev)) Log("Bridge: GPU timing is not available (the rest works)");
    m_inFlight = m_inFlightBefore = 0; m_turn = 0; m_newestSlot = -1; m_releaseCount = 0; m_runs = m_skipped = m_doubled = 0;
    if (m_engine) m_engine->ResetTracking();   // the frames are numbered from 1 again
    m_prevFrameQpc = 0; m_intervalMs = m_lastIntervalMs = m_cpuMs = 0; m_frameTimeCount = 0;
    Log("Bridge: shared fences up");
    return true;
}

void Bridge::Unblock() {
    if (m_copied.d3d12 && m_copied.d3d12->GetCompletedValue() < m_inFlight) {
        Log("Bridge: the frame-copied signal had reached %llu of %llu; signalled from the CPU", (unsigned long long)m_copied.d3d12->GetCompletedValue(),
            (unsigned long long)m_inFlight);
        m_copied.d3d12->Signal(m_inFlight);
    }
    if (m_released.d3d12 && m_released.d3d12->GetCompletedValue() < m_releaseCount) m_released.d3d12->Signal(m_releaseCount);
}

void Bridge::Shutdown() {
    // Runs still queued on the model's side may use the shared textures and fences: release their waits, then let them finish.
    Unblock();
    if (m_engine && (m_input[0].d3d12 || m_copied.d3d12)) m_engine->Drain();
    // a compose on Lossless Scaling's side may wait on the GPU for a result the model did not finish: release it
    if (m_finished.d3d12 && m_finished.d3d12->GetCompletedValue() < m_inFlight) {
        Log("Bridge: the model had finished %llu of %llu; released Lossless Scaling's wait for it", (unsigned long long)m_finished.d3d12->GetCompletedValue(),
            (unsigned long long)m_inFlight);
        m_finished.d3d12->Signal(m_inFlight);
    }
    DropInput(); DropFlow(); DropSlots();
    m_copied.Release(); m_finished.Release(); m_released.Release();
    m_submitTimes.Shutdown(); m_composeTimes.Shutdown();
    if (m_lsPriorityApplied && m_lsPriority != 0) SetLsGpuPriority(0);   // leave Lossless Scaling's device as it was
    SafeRelease(m_ctx4); SafeRelease(m_dev); m_ctx = nullptr;
}

void Bridge::SetLsGpuPriority(int p) {
    if (!m_dev || (m_lsPriorityApplied && p == m_lsPriority)) return;
    IDXGIDevice* dxgi = nullptr;
    if (FAILED(m_dev->QueryInterface(IID_PPV_ARGS(&dxgi)))) return;
    const HRESULT hr = dxgi->SetGPUThreadPriority(p);
    INT now = 0; dxgi->GetGPUThreadPriority(&now);
    dxgi->Release();
    Log("Bridge: Lossless Scaling's GPU thread priority set to %d: 0x%08x (now %d)", p, (unsigned)hr, now);
    m_lsPriority = p; m_lsPriorityApplied = SUCCEEDED(hr);
}

void Bridge::DropInput() { for (SharedTexture& t : m_input) t.Release(); m_fmt = DXGI_FORMAT_UNKNOWN; }
void Bridge::DropFlow() { if (m_engine) m_engine->SetFlowInput(nullptr, 0, 0); for (SharedTexture& t : m_flow) t.Release(); }
void Bridge::DropSlots() { for (Slot& s : m_slots) { s.delta.Release(); s.motion.Release(); s.frame = 0; s.releasedAt = 0; } m_newestSlot = -1; }

bool Bridge::Ensure(uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    const DXGI_FORMAT view = ViewFormat(fmt);
    if (m_input[0].d3d11 && m_input[0].w == w && m_input[0].h == h && m_fmt == view) return true;
    if (!FormatSupported(fmt)) { LogOnce(FailureKey('F', 0, 0, (uint32_t)fmt), "Bridge: frames of format %d cannot be fed to the model", (int)fmt); return false; }
    if (m_input[0].d3d12) m_engine->Drain();
    DropInput(); DropFlow(); DropSlots();
    if (!MakeTexture(m_input[0], w, h, view, false, "input") || !MakeTexture(m_input[1], w, h, view, false, "second input")) { DropInput(); return false; }
    m_fmt = view;
    Log("Bridge: shared input %ux%u, format %d", w, h, (int)view);
    return true;
}

bool Bridge::FitFlow(uint32_t w, uint32_t h) {
    if (m_flow[0].d3d11 && m_flow[0].w == w && m_flow[0].h == h) return true;
    if (m_flow[0].d3d11) { m_engine->Drain(); DropFlow(); }
    if (!MakeTexture(m_flow[0], w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, false, "flow") ||
        !MakeTexture(m_flow[1], w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, false, "second flow")) { DropFlow(); return false; }
    Log("Bridge: shared flow %ux%u, RGBA16F", w, h);
    return true;
}

bool Bridge::FitSlots(uint32_t w, uint32_t h) {
    if (m_slots[0].delta.d3d11 && m_slots[0].delta.w == w && m_slots[0].delta.h == h && (m_slots[0].motion.d3d11 != nullptr) == m_shareMotion) return true;
    m_engine->Drain();
    DropSlots();
    for (Slot& s : m_slots)
        if (!MakeTexture(s.delta, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, "result") ||
            (m_shareMotion && !MakeTexture(s.motion, w, h, DXGI_FORMAT_R16G16_FLOAT, true, "motion"))) { DropSlots(); return false; }
    Log("Bridge: %d shared result slots %ux%u, RGBA16F%s", kSlots, w, h, m_shareMotion ? ", each with its motion vectors (RG16F)" : "");
    return true;
}

int Bridge::FindSlot(uint64_t frame) const {
    for (int i = 0; i < kSlots; ++i) if (m_slots[i].frame == frame) return i;
    return -1;
}

void Bridge::NoteFrameTime(int64_t now, int64_t freq) {
    if (m_prevFrameQpc) {
        const double ms = Ms(now - m_prevFrameQpc, freq);
        if (ms < 500.0) {   // longer is a pause (loading, alt-tab), not a frame time
            m_lastIntervalMs = ms;
            m_intervalMs = Smooth(m_intervalMs, ms);
            if (m_frameTimeCount < kFrameTimeCap) m_frameTimes[m_frameTimeCount++] = (float)ms;
        }
    }
    m_prevFrameQpc = now;
}

// Everything here is recorded on Lossless Scaling's immediate context, in order. The CPU never blocks. Lossless Scaling's queue waits only with
// orderOnGpu (frame generation off), for the model's run before; otherwise the only waits are the model queue's.
bool Bridge::Submit(ID3D11Texture2D* frame, ID3D11Texture2D* flow, uint32_t flowW, uint32_t flowH, const NrParams& params, bool reset, uint64_t frameIndex,
                    bool orderOnGpu) {
    if (!m_input[0].d3d11 || !m_copied.d3d11 || !m_engine) return false;
    if (m_engine->CheckStuck() || !m_engine->IsReady()) return false;   // a model that stopped responding: Lossless Scaling runs untouched
    // The engine's thread is still recording the run before (it takes well under a millisecond): this frame is left out, as when the model is
    // busy. The engine's settings (Prepare, SetFlowInput) are only changed while that thread is idle.
    if (m_engine->Busy()) { ++m_skipped; return false; }
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    const int64_t start = Now();
    NoteFrameTime(start, freq.QuadPart);
    struct CpuTime { Bridge& b; int64_t start, freq; ~CpuTime() { b.m_cpuMs = Smooth(b.m_cpuMs, Ms(Now() - start, freq)); } } cpuTime{ *this, start, freq.QuadPart };

    if (!m_engine->Prepare(m_input[0].w, m_input[0].h, m_fmt, params)) return false;
    if (!FitSlots(m_engine->Stats().workW, m_engine->Stats().workH)) return false;
    // The model is still busy with an earlier frame: skip this one.
    // Room for this frame: the model has finished the run before; or, as long as it keeps up with the frame rate (a run well inside a frame's
    // time, so a second one queued cannot pile up), the input this frame goes into is free (the run before that one has finished).
    const uint64_t finished = m_finished.d3d12->GetCompletedValue();
    const bool busy = m_inFlight && finished < m_inFlight;
    const double runMs = m_engine->Stats().totalMs;
    const bool keepsUp = runMs > 0.0 && m_intervalMs > 0.0 && runMs < 0.75 * m_intervalMs;
    const bool inputFree = finished >= m_inFlightBefore;
    const bool room = !busy || ((keepsUp || orderOnGpu) && inputFree);
    if (!room && !orderOnGpu) { ++m_skipped; return false; }
    m_submitTimes.Begin(m_ctx);
    if (!room) m_ctx4->Wait(m_finished.d3d11, m_inFlightBefore);   // the copy below waits on the GPU until the model has read that input
    m_submitTimes.Mark(m_ctx, 1);
    const bool withFlow = flow && params.useFlow && flowW && flowH && FitFlow(flowW, flowH);
    const int k = m_turn;
    m_engine->SetFlowInput(withFlow ? m_flow[k].d3d12 : nullptr, m_flow[k].w, m_flow[k].h);

    // The slots take turns. The newest holds the result the presents use now; the one before may still be read by a compose already recorded,
    // which is what the run waits on "released" for. So the next in turn is always free to write.
    const int s = (m_newestSlot + 1) % kSlots;
    m_ctx->CopyResource(m_input[k].d3d11, frame);
    if (withFlow) m_ctx->CopyResource(m_flow[k].d3d11, flow);
    m_submitTimes.Mark(m_ctx, 2);
    m_submitTimes.End(m_ctx);
    m_ctx4->Signal(m_copied.d3d11, frameIndex);
    // to the engine's own thread (NrEngine::Submit): it signals "finished" = frameIndex whether or not the run could be queued
    const NrEngine::Job job{ m_input[k].d3d12, m_slots[s].delta.d3d12, m_copied.d3d12, frameIndex, m_released.d3d12, m_slots[s].releasedAt,
                             m_finished.d3d12, frameIndex, reset, m_slots[s].motion.d3d12 };
    m_engine->Submit(job);
    if (busy) ++m_doubled;
    m_inFlightBefore = m_inFlight; m_inFlight = frameIndex; m_newestSlot = s; m_turn = 1 - k;
    m_slots[s].frame = frameIndex;   // its result is used only once the engine says the run went through (RanOk)
    ++m_runs;
    return true;
}

bool Bridge::TakeFrameTimeWindow(float& p50, float& p95, float& p99, float& worst, int& n, int& over20, int& over33) {
    n = m_frameTimeCount;
    if (n < 30) return false;
    float sorted[kFrameTimeCap];
    std::copy(m_frameTimes, m_frameTimes + n, sorted);
    std::sort(sorted, sorted + n);
    auto at = [&](double q) { const int i = (int)(q * n); return sorted[i < n ? i : n - 1]; };
    p50 = at(0.50); p95 = at(0.95); p99 = at(0.99); worst = sorted[n - 1];
    over20 = (int)std::count_if(sorted, sorted + n, [](float ms) { return ms > 20.0f; });
    over33 = (int)std::count_if(sorted, sorted + n, [](float ms) { return ms > 33.0f; });
    m_frameTimeCount = 0;
    return true;
}

uint64_t Bridge::QueuedDelta(ID3D11ShaderResourceView** srv, uint32_t* ww, uint32_t* wh) {
    if (m_newestSlot < 0 || !m_inFlight || m_slots[m_newestSlot].frame != m_inFlight) return 0;   // none
    // a GPU wait on Lossless Scaling's queue follows (BeginDeltaUse): only for a run already on the engine's queue, and one that went through
    if (!m_engine || !m_engine->WaitSubmitted(m_inFlight, 4) || !m_engine->RanOk(m_inFlight)) return 0;
    if (srv) *srv = m_slots[m_newestSlot].delta.view;
    if (ww) *ww = m_slots[m_newestSlot].delta.w;
    if (wh) *wh = m_slots[m_newestSlot].delta.h;
    return m_inFlight;
}

uint64_t Bridge::NewestDelta(ID3D11ShaderResourceView** srv, uint32_t* ww, uint32_t* wh) {
    if (!m_finished.d3d12 || !m_slots[0].delta.d3d11) return 0;
    const uint64_t finished = m_finished.d3d12->GetCompletedValue();
    int newest = -1;
    for (int i = 0; i < kSlots; ++i) {
        const uint64_t f = m_slots[i].frame;
        if (f && f <= finished && m_engine && m_engine->RanOk(f) && (newest < 0 || f > m_slots[newest].frame)) newest = i;
    }
    if (newest < 0) return 0;
    if (srv) *srv = m_slots[newest].delta.view;
    if (ww) *ww = m_slots[newest].delta.w;
    if (wh) *wh = m_slots[newest].delta.h;
    return m_slots[newest].frame;
}

// The result is already finished when a compose reads it, so this wait passes at once; it is there to make the model's writes visible to D3D11.
// Frame generation off, the result is this frame's own, queued just before: the wait is real there, and timed.
void Bridge::BeginDeltaUse(uint64_t d) {
    if (!m_ctx4 || !m_finished.d3d11) return;
    m_composeTimes.Begin(m_ctx);
    m_ctx4->Wait(m_finished.d3d11, d);
    m_composeTimes.Mark(m_ctx, 1);
}
void Bridge::EndDeltaUse(uint64_t d) {
    if (m_ctx) { m_composeTimes.Mark(m_ctx, 2); m_composeTimes.End(m_ctx); }
    const int s = FindSlot(d);
    if (s < 0 || !m_ctx4 || !m_released.d3d11) return;
    m_ctx4->Signal(m_released.d3d11, ++m_releaseCount);
    m_slots[s].releasedAt = m_releaseCount;
}
