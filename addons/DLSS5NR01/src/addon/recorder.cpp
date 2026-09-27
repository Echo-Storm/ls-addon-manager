#include "addon/recorder.h"
#include "addon/bridge.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace nr {

namespace {
template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
std::wstring Wide(const std::string& s) {
    std::wstring w(s.size(), L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    w.resize(n > 0 ? n : 0);
    return w;
}
std::string Utf8(const std::wstring& w) {
    std::string s(w.size() * 3, '\0');
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    s.resize(n > 0 ? n : 0);
    return s;
}
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
void Recorder::Collect(bool waitForWorkers) {
    ID3D11DeviceContext* const ctx = m_ctx;
    if (!ctx) return;
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
        if (!waitForWorkers || !busy) return;
        if (m_workers.empty()) {   // nothing will finish them: give the memory back as it is
            for (Slot& s : m_slots) if (s.state == Slot::Working) { ctx->Unmap(s.staging, 0); s.state = Slot::Free; }
            return;
        }
        Sleep(1);
    }
    Log("recorder: the compression threads did not finish in time");
}

void Recorder::DropStaging() {
    for (Slot& s : m_slots) {
        if (s.state == Slot::Working && m_ctx) m_ctx->Unmap(s.staging, 0);
        SafeRelease(s.staging); s.state = Slot::Free; s.done = false;
    }
    SafeRelease(m_ctx); SafeRelease(m_dev);
}

void Recorder::Offer(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, uint32_t source, uint32_t content, uint32_t tag) {
    if (!ctx || !frame) return;
    const bool on = m_on.load();
    if (!on && !m_dev) return;   // off, and nothing to give back
    Collect(false);
    if (!on) { Collect(true); DropStaging(); StopWorkers(); return; }

    D3D11_TEXTURE2D_DESC d; frame->GetDesc(&d);
    const DXGI_FORMAT view = Bridge::ViewFormat(d.Format);
    const uint32_t bpp = lsrec::BytesPerPixel(view);
    if (!bpp || d.SampleDesc.Count != 1) {
        static DXGI_FORMAT said = DXGI_FORMAT_UNKNOWN;
        if (said != d.Format) { said = d.Format; Log("recorder: frames of format %d (or multisampled) cannot be recorded", (int)d.Format); }
        return;
    }
    ID3D11Device* dev = nullptr; frame->GetDevice(&dev);
    if (d.Width != m_w || d.Height != m_h || d.Format != m_fmt || source != m_source || content != m_content || dev != m_dev || ctx != m_ctx) {
        Collect(true); DropStaging();
        m_dev = dev; m_dev->AddRef(); m_ctx = ctx; m_ctx->AddRef();
        {
            std::lock_guard<std::mutex> lock(m_keepMutex);
            m_kept.clear(); m_keptBytes = 0; ++m_keptGeneration;
            m_w = d.Width; m_h = d.Height; m_fmt = d.Format; m_viewFmt = view; m_bpp = bpp; m_source = source; m_content = content;
        }
        Log("recorder: %ux%u, format %d (%u bytes a pixel), %s", m_w, m_h, (int)view, bpp,
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
    ctx->CopySubresourceRegion(slot->staging, 0, 0, 0, 0, frame, 0, nullptr);
    slot->index = index; slot->qpc = Qpc(); slot->tag = tag; slot->state = Slot::Copied;
}

void Recorder::Forget() {
    Collect(true);
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

bool Recorder::Save(const std::wstring& folder, const std::string& game) {
    if (m_saving.exchange(true)) return false;
    std::vector<std::shared_ptr<lsrec::Frame>> frames;
    lsrec::FileHeader header;
    {
        std::lock_guard<std::mutex> lock(m_keepMutex);
        frames.assign(m_kept.begin(), m_kept.end());
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
        if (ok) snprintf(text, sizeof text, "Saved %.1f s (%zu frames, %.0f MB) to %s", seconds, frames.size(), bytes / 1048576.0, Utf8(path).c_str());
        else snprintf(text, sizeof text, "Could not save %s: %s", Utf8(path).c_str(), error.c_str());
        Log("recorder: %s", text);
        { std::lock_guard<std::mutex> lock(m_resultMutex); m_result = text; m_resultOk = ok; }
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

std::string Recorder::LastResult(bool& ok) const {
    std::lock_guard<std::mutex> lock(m_resultMutex);
    ok = m_resultOk;
    return m_result;
}

} // namespace nr
