#include "addon/recorder.h"
#include "addon/hdr.h"
#include "addon/text_util.h"
#include <windows.h>
#include <d3dcompiler.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>

#pragma comment(lib, "d3dcompiler.lib")

namespace nr {

namespace {
template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
// the UNORM / FLOAT format to view a frame of format f with (sRGB and typeless map to it); the same as Bridge::ViewFormat, which this file no longer pulls in
DXGI_FORMAT ViewFormatOf(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
using nr::util::Wide;
using nr::util::Utf8;
} // namespace

void Recorder::Log(const char* fmt, ...) {
    if (!m_log) return;
    char text[512]; va_list args; va_start(args, fmt); vsnprintf(text, sizeof text, fmt, args); va_end(args);
    m_log(text);
}

void Recorder::Configure(bool on, float seconds, uint32_t budgetMb) {
    m_seconds = std::clamp(seconds, 1.0f, 60.0f);
    m_budget = static_cast<uint64_t>(std::clamp(budgetMb, 256u, 65536u)) << 20;
    const bool was = m_on.exchange(on);
    if (was == on) return;
    if (!on) {   // the frames go now; the staging textures at the next Offer, on the render thread
        std::lock_guard<std::mutex> lock(m_keepMutex);
        m_kept.clear(); m_keptBytes = 0; ++m_keptGeneration;
    }
    Log("recorder: %s (the last %.0f s, at most %u MB)", on ? "on" : "off", m_seconds.load(), static_cast<unsigned>(m_budget.load() >> 20));
}

void Recorder::StartWorkers() {
    if (!m_workers.empty()) return;
    m_stop = false;
    const unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 6u);
    for (unsigned i = 0; i < n; ++i) m_workers.emplace_back([this] { Worker(); });
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); m_freq = f.QuadPart;
    Log("recorder: %u threads compress the frames", n);
}

void Recorder::StopWorkers() {
    { std::lock_guard<std::mutex> lock(m_jobMutex); m_stop = true; }
    m_jobCv.notify_all();
    for (std::thread& t : m_workers) if (t.joinable()) t.join();
    m_workers.clear();
    m_jobs.clear();
}

void Recorder::Worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   // the game's and Lossless Scaling's threads come first
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobCv.wait(lock, [&] { return m_stop || !m_jobs.empty(); });
            if (m_jobs.empty()) return;   // stopping, nothing left
            job = m_jobs.front(); m_jobs.pop_front();
        }
        Slot& s = *job.slot;
        auto frame = std::make_shared<lsrec::Frame>();
        frame->header.index = s.index; frame->header.qpc = s.qpc; frame->header.codec = lsrec::kQoi; frame->header.tag = s.tag;
        frame->header.rawBytes = job.w * job.h * job.bpp;
        frame->data.reserve(frame->header.rawBytes / 2);
        lsrec::Compress(static_cast<const uint8_t*>(s.mapped.pData), job.w * job.bpp / 4, job.h, s.mapped.RowPitch, frame->data);
        frame->header.bytes = static_cast<uint32_t>(frame->data.size());
        frame->data.shrink_to_fit();
        s.done = true;   // the render thread may unmap it now
        if (job.generation == m_keptGeneration.load()) Keep(std::move(frame));
    }
}

void Recorder::Keep(std::shared_ptr<lsrec::Frame> frame) {
    std::lock_guard<std::mutex> lock(m_keepMutex);
    if (!m_on) return;
    auto at = m_kept.end();   // the workers may finish out of order: kept in order of index
    while (at != m_kept.begin() && (*(at - 1))->header.index > frame->header.index) --at;
    m_keptBytes += frame->data.size();
    m_kept.insert(at, std::move(frame));
    const int64_t span = static_cast<int64_t>(m_seconds.load() * static_cast<double>(m_freq));
    while (m_kept.size() > 1 && (m_kept.front()->header.qpc < m_kept.back()->header.qpc - span || m_keptBytes > m_budget.load())) {
        m_keptBytes -= m_kept.front()->data.size();
        m_kept.pop_front();
    }
}

// Unmaps what the workers are done with and maps what the GPU has finished copying (without waiting, unless waitForWorkers: then until every
// staging texture is free, for a change of size or the end).
bool Recorder::Collect(bool waitForWorkers) {
    ID3D11DeviceContext* const ctx = m_ctx;
    if (!ctx) return true;
    for (int round = 0; round < 4000; ++round) {
        bool busy = false;
        for (Slot& s : m_slots) {
            if (s.state == Slot::Working && s.done) { ctx->Unmap(s.staging, 0); s.state = Slot::Free; s.done = false; }
            if (s.state == Slot::Copied) {
                if (SUCCEEDED(ctx->Map(s.staging, 0, D3D11_MAP_READ, waitForWorkers ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &s.mapped))) {
                    s.state = Slot::Working; s.done = false;
                    { std::lock_guard<std::mutex> lock(m_jobMutex); m_jobs.push_back({ &s, m_keptGeneration.load(), m_w, m_h, m_bpp }); }
                    m_jobCv.notify_one();
                }
            }
            busy = busy || s.state != Slot::Free;
        }
        if (!busy) return true;
        if (!waitForWorkers) return false;
        if (m_workers.empty()) {   // nothing will finish them: give the memory back as it is
            for (Slot& s : m_slots) if (s.state == Slot::Working) { ctx->Unmap(s.staging, 0); s.state = Slot::Free; }
            return true;
        }
        Sleep(1);
    }
    Log("recorder: the compression threads did not finish in time; the staging memory is left alone until they do");
    return false;
}

// Frames of more than 8 bits as an 8-bit SDR picture: scRGB (1.0 = 80 nits; the display's SDR white maps to 1) and HDR10 (PQ) are brought to the SDR white and encoded with
// the sRGB curve (highlights above the SDR white clip); a 10-bit SDR frame is already encoded.
static const char kConvertHlsl[] = R"(
cbuffer C : register(b0) { uint mode; uint w; uint h; uint pad; float scale; float3 pad2; };
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
float3 SrgbEncode(float3 c) { c = saturate(c); return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float3 PqToNits(float3 e) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(max(e, 0.0), 1.0 / m2);
    return 10000.0 * pow(max(max(p - c1, 0.0) / (c2 - c3 * p), 0.0), 1.0 / m1);
}
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= w || id.y >= h) return;
    float3 c = src.Load(int3(id.xy, 0)).rgb;
    float3 rgb = mode == 1 ? SrgbEncode(c * scale) : mode == 2 ? SrgbEncode(PqToNits(c) * scale) : saturate(c);
    dst[id.xy] = float4(rgb, 1.0);
}
)";

void Recorder::DropConvert() {
    SafeRelease(m_conv.srv); SafeRelease(m_conv.copy); SafeRelease(m_conv.uav); SafeRelease(m_conv.out);
    SafeRelease(m_conv.cs); SafeRelease(m_conv.cb); SafeRelease(m_conv.deferred);
    m_conv.w = m_conv.h = 0; m_conv.fmt = DXGI_FORMAT_UNKNOWN;
}

// On the frames' device, through a deferred context that is executed with the immediate context's state restored: this runs inside one of Lossless Scaling's own passes (before
// it dispatches, with its shader and resources already bound), and nothing of that may change.
bool Recorder::ConvertTo8(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, const D3D11_BOX* box, uint32_t mode, float scale) {
    Convert& c = m_conv;
    if (!m_dev) return false;
    if (!c.out || c.w != m_w || c.h != m_h || c.fmt != m_srcFmt) {
        DropConvert();
        D3D11_TEXTURE2D_DESC cd{}; cd.Width = m_w; cd.Height = m_h; cd.MipLevels = 1; cd.ArraySize = 1; cd.Format = m_srcFmt; cd.SampleDesc.Count = 1; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_TEXTURE2D_DESC od = cd; od.Format = DXGI_FORMAT_R8G8B8A8_UNORM; od.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = m_srcView; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uv{}; uv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; uv.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
        const bool ok = SUCCEEDED(m_dev->CreateTexture2D(&cd, nullptr, &c.copy)) && SUCCEEDED(m_dev->CreateShaderResourceView(c.copy, &sv, &c.srv)) &&
                        SUCCEEDED(m_dev->CreateTexture2D(&od, nullptr, &c.out)) && SUCCEEDED(m_dev->CreateUnorderedAccessView(c.out, &uv, &c.uav)) &&
                        SUCCEEDED(m_dev->CreateBuffer(&bd, nullptr, &c.cb)) && SUCCEEDED(m_dev->CreateDeferredContext(0, &c.deferred)) &&
                        SUCCEEDED(D3DCompile(kConvertHlsl, sizeof kConvertHlsl - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err)) &&
                        SUCCEEDED(m_dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &c.cs));
        if (code) code->Release();
        if (err) { Log("recorder: the conversion shader: %s", static_cast<const char*>(err->GetBufferPointer())); err->Release(); }
        if (!ok) { Log("recorder: the 8-bit conversion could not be set up: frames are not recorded"); DropConvert(); return false; }
        c.w = m_w; c.h = m_h; c.fmt = m_srcFmt;
    }
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(c.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return false;
    struct Cb { uint32_t mode, w, h, pad; float scale, pad2[3]; } cb{ mode, m_w, m_h, 0, scale, { 0, 0, 0 } };
    memcpy(map.pData, &cb, sizeof cb);
    ctx->Unmap(c.cb, 0);
    c.deferred->CopySubresourceRegion(c.copy, 0, 0, 0, 0, frame, 0, box);
    ID3D11ShaderResourceView* const srv = c.srv; ID3D11UnorderedAccessView* const uav = c.uav; ID3D11Buffer* const buffer = c.cb; const UINT keep = ~0u;
    c.deferred->CSSetShader(c.cs, nullptr, 0);
    c.deferred->CSSetConstantBuffers(0, 1, &buffer);
    c.deferred->CSSetShaderResources(0, 1, &srv);
    c.deferred->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
    c.deferred->Dispatch((m_w + 15) / 16, (m_h + 15) / 16, 1);
    ID3D11CommandList* list = nullptr;
    if (FAILED(c.deferred->FinishCommandList(FALSE, &list)) || !list) return false;
    ctx->ExecuteCommandList(list, TRUE);   // TRUE: the immediate context's state is put back as it was
    list->Release();
    return true;
}

void Recorder::DropStaging() {
    DropConvert();
    for (Slot& s : m_slots) {
        if (s.state == Slot::Working && m_ctx) m_ctx->Unmap(s.staging, 0);
        SafeRelease(s.staging); s.state = Slot::Free; s.done = false;
    }
    SafeRelease(m_ctx); SafeRelease(m_dev);
}

void Recorder::Offer(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, uint32_t source, uint32_t content, uint32_t tag, uint32_t cropW, uint32_t cropH) {
    if (!ctx || !frame) return;
    const bool on = m_on.load();
    if (!on && !m_dev) return;   // off, and nothing to give back
    Collect(false);
    if (!on) { if (Collect(true)) { DropStaging(); StopWorkers(); } return; }   // (not all free: the workers are still reading; tried again with the next frame)

    D3D11_TEXTURE2D_DESC d; frame->GetDesc(&d);
    const DXGI_FORMAT view = ViewFormatOf(d.Format);
    const uint32_t bpp = lsrec::BytesPerPixel(view);
    // Compact: more than 8 bits (HDR) is made into an 8-bit SDR picture on the card; an 8-bit frame is kept as it is
    const bool convert = m_compact.load() && bpp && view != DXGI_FORMAT_R8G8B8A8_UNORM && view != DXGI_FORMAT_B8G8R8A8_UNORM;
    const DXGI_FORMAT recFmt = convert ? DXGI_FORMAT_R8G8B8A8_UNORM : d.Format, recView = convert ? DXGI_FORMAT_R8G8B8A8_UNORM : view;
    const uint32_t recBpp = convert ? 4u : bpp, recContent = convert ? static_cast<uint32_t>(lsrec::kSdrView) : content;
    if (!bpp || d.SampleDesc.Count != 1) {
        static DXGI_FORMAT said = DXGI_FORMAT_UNKNOWN;
        if (said != d.Format) { said = d.Format; Log("recorder: frames of format %d (or multisampled) cannot be recorded", (int)d.Format); }
        return;
    }
    ID3D11Device* dev = nullptr; frame->GetDevice(&dev);
    const uint32_t keepW = cropW && cropW < d.Width ? cropW : d.Width, keepH = cropH && cropH < d.Height ? cropH : d.Height;
    if (d.Width != m_srcW || d.Height != m_srcH || keepW != m_w || keepH != m_h || d.Format != m_srcFmt || recFmt != m_fmt || source != m_source || recContent != m_content || dev != m_dev || ctx != m_ctx) {
        if (!Collect(true)) { if (dev) dev->Release(); ++m_missed; return; }   // the workers still read the old staging memory: this frame is left out, the change is made when they are done
        DropStaging();
        m_dev = dev; m_dev->AddRef(); m_ctx = ctx; m_ctx->AddRef();
        {
            std::lock_guard<std::mutex> lock(m_keepMutex);
            m_kept.clear(); m_keptBytes = 0; ++m_keptGeneration;
            m_srcW = d.Width; m_srcH = d.Height; m_w = keepW; m_h = keepH; m_srcFmt = d.Format; m_srcView = view; m_fmt = recFmt; m_viewFmt = recView; m_bpp = recBpp; m_source = source; m_content = recContent; m_convert = convert;
        }
        Log("recorder: %ux%u%s, format %d (%u bytes a pixel)%s, %s", m_w, m_h, (m_w != m_srcW || m_h != m_srcH) ? " (the middle of the frame)" : "", (int)recView, recBpp,
            convert ? " (compact: an 8-bit SDR picture)" : "",
            source == lsrec::kCaptured ? "the captured frames" : source == lsrec::kPresented ? "the presented frames" : "NIS's input");
    }
    if (dev) dev->Release();
    StartWorkers();
    const uint64_t index = ++m_offered;
    Slot* slot = nullptr;
    for (Slot& s : m_slots) if (s.state == Slot::Free) { slot = &s; break; }
    if (!slot) { ++m_missed; return; }
    if (!slot->staging) {
        D3D11_TEXTURE2D_DESC sd{}; sd.Width = m_w; sd.Height = m_h; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = m_fmt; sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m_dev->CreateTexture2D(&sd, nullptr, &slot->staging))) { ++m_missed; Log("recorder: a staging texture could not be made"); return; }
    }
    const bool cropped = m_w != m_srcW || m_h != m_srcH;
    const UINT left = ((m_srcW - m_w) / 2) & ~1u, top = ((m_srcH - m_h) / 2) & ~1u;   // the middle (an even offset: the formats' blocks)
    const D3D11_BOX box{ left, top, 0, left + m_w, top + m_h, 1 };
    if (m_convert) {
        const DisplayHdr display = QueryDisplayHdr(nullptr, m_dev);
        const FrameEncoding e = EncodingOf(m_srcView, 0, display.hdr);
        const uint32_t mode = e == FrameEncoding::ScRgb ? 1u : e == FrameEncoding::Hdr10 ? 2u : 0u;
        const float white = display.whiteNits > 1.0f ? display.whiteNits : 1.0f;
        const float scale = e == FrameEncoding::ScRgb ? 80.0f / white : 1.0f / white;
        if (!ConvertTo8(ctx, frame, cropped ? &box : nullptr, mode, scale)) { ++m_missed; return; }
        ctx->CopyResource(slot->staging, m_conv.out);
    } else if (cropped) {
        ctx->CopySubresourceRegion(slot->staging, 0, 0, 0, 0, frame, 0, &box);
    } else ctx->CopySubresourceRegion(slot->staging, 0, 0, 0, 0, frame, 0, nullptr);
    slot->index = index; slot->qpc = Qpc(); slot->tag = tag; slot->state = Slot::Copied;
}

void Recorder::Forget() {
    if (!Collect(true)) StopWorkers();   // the workers did not finish in time: join them before the staging memory goes (they read it through the mapped pointers)
    DropStaging();
    StopWorkers();
    m_w = m_h = 0; m_fmt = DXGI_FORMAT_UNKNOWN;
}

void Recorder::Shutdown() {
    StopWorkers();
    for (Slot& s : m_slots) { SafeRelease(s.staging); s.state = Slot::Free; }   // Forget came first; else the device is going and nothing is unmapped
    SafeRelease(m_ctx); SafeRelease(m_dev);
    if (m_saver.joinable()) m_saver.join();
}

bool Recorder::Save(const std::wstring& folder, const std::string& game, bool clear) {
    if (m_saving.exchange(true)) return false;
    std::vector<std::shared_ptr<lsrec::Frame>> frames;
    lsrec::FileHeader header;
    {
        std::lock_guard<std::mutex> lock(m_keepMutex);
        frames.assign(m_kept.begin(), m_kept.end());
        if (clear && !frames.empty()) { m_kept.clear(); m_keptBytes = 0; }
        header.width = m_w; header.height = m_h; header.format = m_viewFmt; header.bytesPerPixel = m_bpp; header.source = m_source; header.content = m_content;
    }
    if (frames.empty()) {
        m_saving = false;
        std::lock_guard<std::mutex> lock(m_resultMutex); m_result = "Nothing recorded yet"; m_resultOk = false;
        return false;
    }
    header.qpcFrequency = m_freq;
    snprintf(header.game, sizeof header.game, "%s", game.c_str());
    if (m_saver.joinable()) m_saver.join();
    m_saver = std::thread([this, frames = std::move(frames), header, folder, game] {
        SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
        std::wstring name = Wide(game.empty() ? std::string("Lossless Scaling") : game);
        if (const size_t dot = name.rfind(L'.'); dot != std::wstring::npos && dot > 0) name.resize(dot);
        SYSTEMTIME t; GetLocalTime(&t);
        wchar_t stamp[40]; swprintf(stamp, 40, L"_%04u-%02u-%02u_%02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        std::wstring path = folder + L"\\" + name + stamp + L".lsrec";
        for (int n = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && n < 100; ++n) path = folder + L"\\" + name + stamp + L"-" + std::to_wstring(n) + L".lsrec";
        std::vector<const lsrec::Frame*> list; uint64_t bytes = 0;
        for (const auto& f : frames) { list.push_back(f.get()); bytes += f->data.size() + sizeof(lsrec::FrameHeader); }
        std::string error;
        const bool ok = lsrec::Write(path, header, list, &error);
        const double seconds = frames.size() > 1 && header.qpcFrequency ? static_cast<double>(frames.back()->header.qpc - frames.front()->header.qpc) / header.qpcFrequency : 0.0;
        char text[768];
        const uint64_t left = m_missed.load();   // frames offered while every staging texture was busy (the card or the processor could not keep up), since recording started
        if (ok) snprintf(text, sizeof text, "Saved %.1f s (%zu frames, %.0f MB) to %s%s", seconds, frames.size(), bytes / 1048576.0, Utf8(path).c_str(), left ? " (some frames were left out: the card or the processor could not keep up; see the missed count in the panel)" : "");
        else snprintf(text, sizeof text, "Could not save %s: %s", Utf8(path).c_str(), error.c_str());
        Log("recorder: %s", text);
        {
            std::lock_guard<std::mutex> lock(m_resultMutex); m_result = text; m_resultOk = ok;
            if (!ok) ++m_fails;
            if (ok) {
                m_lastSave = SaveInfo{ path, static_cast<uint32_t>(frames.size()), seconds, bytes, header.width, header.height, header.format, header.source, left };
                ++m_saves;
            }
        }
        m_saving = false;
    });
    return true;
}

Recorder::Status Recorder::GetStatus() const {
    Status s;
    s.on = m_on.load(); s.missed = m_missed.load(); s.saving = m_saving.load();
    std::lock_guard<std::mutex> lock(m_keepMutex);
    s.frames = static_cast<uint32_t>(m_kept.size()); s.bytes = m_keptBytes; s.w = m_w; s.h = m_h;
    if (m_kept.size() > 1 && m_freq) s.seconds = static_cast<double>(m_kept.back()->header.qpc - m_kept.front()->header.qpc) / m_freq;
    return s;
}

bool Recorder::LastSave(SaveInfo& info) const {
    std::lock_guard<std::mutex> lock(m_resultMutex);
    if (!m_saves.load()) return false;
    info = m_lastSave;
    return true;
}

std::string Recorder::LastResult(bool& ok) const {
    std::lock_guard<std::mutex> lock(m_resultMutex);
    ok = m_resultOk;
    return m_result;
}

} // namespace nr
