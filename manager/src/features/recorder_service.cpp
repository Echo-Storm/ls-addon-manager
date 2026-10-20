#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "recorder_service.h"
#include "addon/scaler11.h"   // FindNisPass: the pass that scales the captured frame (frame generation off)
#include <windows.h>
#include <mmsystem.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace eam {
namespace rec {

namespace {

// The sounds: a short tone built in memory and played through the normal audio path (PlaySound), not Beep(): Beep did not come out of the owner's speakers (a TV on HDMI, a
// game running). A tick when the key is pressed (the file may take a minute to write: 8 GB of HDR), a rising pair when it is saved, a low one when there was nothing to save.
struct Tone { int hz; int ms; };
void PlayTones(const Tone* tones, int count) {
    static std::mutex mutex;   // the buffer must stay valid while the sound plays: one buffer, replaced by the next sound
    static std::vector<uint8_t> wav;
    std::lock_guard<std::mutex> lock(mutex);
    constexpr int kRate = 22050;
    std::vector<int16_t> pcm;
    for (int t = 0; t < count; ++t) {
        const int n = kRate * tones[t].ms / 1000, fade = std::max(1, kRate / 200);   // a 5 ms fade at each end: no click
        for (int i = 0; i < n; ++i) {
            const double env = std::min({ 1.0, static_cast<double>(i) / fade, static_cast<double>(n - i) / fade });
            pcm.push_back(static_cast<int16_t>(std::sin(6.283185307179586 * tones[t].hz * i / kRate) * 0.45 * env * 32767.0));
        }
    }
    const uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * 2), total = 44 + dataBytes;
    wav.assign(total, 0);
    auto put32 = [&](size_t at, uint32_t v) { memcpy(&wav[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { memcpy(&wav[at], &v, 2); };
    memcpy(&wav[0], "RIFF", 4); put32(4, total - 8); memcpy(&wav[8], "WAVEfmt ", 8); put32(16, 16); put16(20, 1); put16(22, 1);
    put32(24, kRate); put32(28, kRate * 2); put16(32, 2); put16(34, 16); memcpy(&wav[36], "data", 4); put32(40, dataBytes);
    if (dataBytes) memcpy(&wav[44], pcm.data(), dataBytes);
    PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}
void SoundPress() { const Tone t[] = { { 1000, 70 } }; PlayTones(t, 1); }
void SoundSaved() { const Tone t[] = { { 660, 90 }, { 880, 130 } }; PlayTones(t, 2); }
void SoundNothing() { const Tone t[] = { { 300, 300 } }; PlayTones(t, 1); }

constexpr uint64_t kClipMs = 10000;
constexpr uint64_t kTapQuietMs = 2000;       // a context that has had no captured frame for this long gives up the recording to another
constexpr int kMaxTaps = 8;

std::wstring Wide(const std::string& s) {
    std::wstring w(s.size(), L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    w.resize(n > 0 ? n : 0);
    return w;
}
std::string Utf8(const std::wstring& w) {
    std::string s(w.size() * 3 + 1, '\0');
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    s.resize(n > 0 ? n : 0);
    return s;
}
std::string JsonString(const std::string& s) {
    std::string out = "\"";
    for (const unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
            else out += static_cast<char>(c);
        }
    }
    return out + "\"";
}
const char* SourceName(uint32_t source) {
    return source == nr::lsrec::kCaptured ? "captured" : source == nr::lsrec::kPresented ? "presented" : "nis_input";
}

} // namespace

Service::Service() = default;
Service::~Service() { Shutdown(); }

void Service::SetLog(LogFn log) { m_log = log; m_recorder.SetLog(std::move(log)); }
void Service::SetInfoProvider(std::function<std::string()> provider) { m_info = std::move(provider); }

void Service::Log(const char* fmt, ...) {
    if (!m_log) return;
    char text[512]; va_list args; va_start(args, fmt); vsnprintf(text, sizeof text, fmt, args); va_end(args);
    m_log(text);
}

Settings Service::Current() const { std::lock_guard<std::mutex> lock(m_settingsMutex); return m_settings; }

uint32_t Service::Flags() const { return (m_on.load() ? kFlagOn : 0u) | (m_shown.load() ? kFlagShown : 0u); }

void Service::ApplyToRecorder() {
    Settings s; { std::lock_guard<std::mutex> lock(m_settingsMutex); s = m_settings; }
    const bool clipping = m_clipUntil.load() != 0 && !s.on;
    const bool on = s.on || clipping;
    // a clip keeps all ten seconds, in as much memory as they take (4K HDR is big)
    m_recorder.Configure(on, clipping ? std::max(s.seconds, kClipMs / 1000.0f) : s.seconds,
                         static_cast<uint32_t>(clipping ? std::max(s.budgetMb, 8192) : s.budgetMb));
    m_shown = s.shown; m_saveAfter = s.saveAfter;
    m_on = on;
}

void Service::Configure(const Settings& s) {
    { std::lock_guard<std::mutex> lock(m_settingsMutex); m_settings = s; }
    if (!s.on && m_clipUntil.load() == 0 && m_saveAt.load() == 0) m_savedForTest = false;
    ApplyToRecorder();
}

// ---- the captured frames

void Service::OnDispatchImpl(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z) {
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(m_tapMutex);
    auto it = m_taps.find(ctx);
    if (it == m_taps.end()) {
        if (static_cast<int>(m_taps.size()) >= kMaxTaps) return;
        it = m_taps.emplace(ctx, TapState{}).first;
    }
    TapState& st = it->second;
    if (!st.tap) { st.tap = std::make_unique<FrameTap>(); st.firstMs = now; }
    st.lastMs = now;
    TapDecision d;
    st.tap->Observe(ctx, x, y, z, d);   // (what it hands over for a model is not wanted here: only the frame this very pass reads)
    ID3D11Texture2D* tex = nullptr;
    const bool mine = m_owner == ctx || !m_owner || now - m_ownerMs > kTapQuietMs;
    if (d.isTap && d.tapFrame && mine && SUCCEEDED(d.tapFrame->QueryInterface(IID_PPV_ARGS(&tex)))) {
        D3D11_TEXTURE2D_DESC desc; tex->GetDesc(&desc);
        if (desc.Width >= 64 && desc.Height >= 64) {
            m_owner = ctx; m_ownerMs = now; m_lastTapOfferMs = now;
            std::lock_guard<std::mutex> rec(m_recMutex);
            m_recorder.Offer(ctx, tex, nr::lsrec::kCaptured, 0, 0, 0, 0);
            const int after = m_saveAfter.load();
            if (after > 0 && !m_savedForTest && m_recorder.GetStatus().frames >= static_cast<uint32_t>(after)) { m_savedForTest = true; m_recorder.Save(Folder(), GameName()); }
        }
        tex->Release();
    }
    if (d.frame) d.frame->Release();
    if (d.flow) d.flow->Release();
    d.frame = nullptr; d.flow = nullptr;
    // Frame generation OFF: Lossless Scaling runs no frame generation passes, so the tap above finds nothing; the frame it captured from the game is what its own scaling
    // pass (NIS) reads, so that pass's input is recorded instead. Only after a few seconds without a TAP on this context (the tap needs a moment to learn the passes, and with frame
    // generation on the scaling pass reads the generated frames too), and never while the tap is offering.
    if (!st.tap->Taps() && now - st.firstMs > 3000 && now - m_lastTapOfferMs.load() > 1500 && (m_owner == ctx || !m_owner || now - m_ownerMs > kTapQuietMs)) {
        nr::NisPass pass;
        if (nr::FindNisPass(ctx, x, y, z, pass)) {
            ID3D11Texture2D* in = nullptr;
            if (pass.in && SUCCEEDED(pass.in->QueryInterface(IID_PPV_ARGS(&in)))) {
                D3D11_TEXTURE2D_DESC desc; in->GetDesc(&desc);
                if (desc.Width >= 64 && desc.Height >= 64) {
                    m_owner = ctx; m_ownerMs = now; m_noTapRecording = true;
                    std::lock_guard<std::mutex> rec(m_recMutex);
                    m_recorder.Offer(ctx, in, nr::lsrec::kCaptured, 0, 0, 0, 0);
                    const int after = m_saveAfter.load();
                    if (after > 0 && !m_savedForTest && m_recorder.GetStatus().frames >= static_cast<uint32_t>(after)) { m_savedForTest = true; m_recorder.Save(Folder(), GameName()); }
                }
                in->Release();
            }
            nr::ReleaseNisPass(pass);
        }
    }
    if (now - m_lastCleanMs > 2000) {   // a context that has gone (Lossless Scaling makes and drops devices as scaling starts and stops): its tap lets go of the textures it holds
        m_lastCleanMs = now;
        for (auto i = m_taps.begin(); i != m_taps.end();) {
            if (i->first != ctx && now - i->second.lastMs > 5000) { if (m_owner == i->first) m_owner = nullptr; i = m_taps.erase(i); }
            else ++i;
        }
    }
}

namespace {
// no objects with destructors here: __try cannot unwind them
bool GuardedDispatch(Service* s, void (Service::*fn)(ID3D11DeviceContext*, uint32_t, uint32_t, uint32_t), ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z) {
    __try { (s->*fn)(ctx, x, y, z); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

void Service::OnDispatch(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z) {
    if (!m_on.load(std::memory_order_relaxed) || m_shown.load(std::memory_order_relaxed) || m_faulted || !ctx) return;
    if (!GuardedDispatch(this, &Service::OnDispatchImpl, ctx, x, y, z)) {
        m_faulted = true; m_on = false;   // a fault in the tap must never reach Lossless Scaling again: the recorder goes off until it is switched on again
        Log("recorder: a fault in the frame tap: the recorder is off (switch it off and on again to retry)");
    }
}

// ---- frames from the addons

void Service::Offer(ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, uint32_t source, uint32_t content, uint32_t tag, uint32_t cropW, uint32_t cropH) {
    if (!m_on.load() || !ctx || !frame || source == nr::lsrec::kCaptured) return;
    if (!m_shown.load() && source == nr::lsrec::kPresented && GetTickCount64() - m_lastTapOfferMs.load() < 1500) return;   // the tap has the captured frames
    std::lock_guard<std::mutex> rec(m_recMutex);
    m_recorder.Offer(ctx, frame, source, content, tag, cropW, cropH);
    const int after = m_saveAfter.load();
    if (after > 0 && !m_savedForTest && m_recorder.GetStatus().frames >= static_cast<uint32_t>(after)) { m_savedForTest = true; m_recorder.Save(Folder(), GameName()); }
}

// ---- saving

std::wstring Service::Folder() const {
    std::wstring folder; { std::lock_guard<std::mutex> lock(m_settingsMutex); folder = m_settings.folder; }
    if (!folder.empty()) { for (wchar_t& c : folder) if (c == L'/') c = L'\\'; return folder; }
    PWSTR videos = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos)) && videos) { folder = videos; CoTaskMemFree(videos); }
    else folder = L".";
    if (folder.find(L"\\OneDrive") != std::wstring::npos) {   // Videos moved into OneDrive would upload every recording (gigabytes): the profile's own Videos folder instead
        PWSTR profile = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile)) && profile) { folder = std::wstring(profile) + L"\\Videos"; CoTaskMemFree(profile); }
    }
    return folder + L"\\Lossless Scaling";
}

void Service::NoteForeground() {
    HWND const w = GetForegroundWindow();
    if (!w) return;
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    if (!pid || pid == GetCurrentProcessId()) return;
    HANDLE const h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return;
    wchar_t path[MAX_PATH] = L""; DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, path, &n)) {
        const wchar_t* base = wcsrchr(path, L'\\'); base = base ? base + 1 : path;
        std::lock_guard<std::mutex> lock(m_gameMutex);
        m_game = Utf8(base);
    }
    CloseHandle(h);
}

std::string Service::GameName() {
    std::lock_guard<std::mutex> lock(m_gameMutex);
    return m_game;
}

bool Service::Save() {
    std::lock_guard<std::mutex> rec(m_recMutex);
    return m_recorder.Save(Folder(), GameName());
}

int Service::SaveNamed(const std::string& name, bool clear) {
    std::lock_guard<std::mutex> rec(m_recMutex);
    if (!m_on.load()) return 0;
    if (m_recorder.Save(Folder(), name.empty() ? GameName() : name, clear)) return 1;
    return m_recorder.GetStatus().saving ? 2 : 0;
}

void Service::TestSound() { const Tone t[] = { { 1000, 70 }, { 0, 220 }, { 660, 90 }, { 880, 130 } }; PlayTones(t, 4); }

void Service::Clip() {
    if (m_clipUntil.load() != 0) return;
    m_clipUntil = GetTickCount64() + kClipMs + 500;
    ApplyToRecorder();
    Log("recorder: recording the next %.0f s, then saving", kClipMs / 1000.0);
}

bool Service::Request() {
    const Settings s = Current();
    if (!m_on.load()) { Clip(); return true; }
    if (s.afterSeconds > 0.05f) {
        if (m_saveAt.load() == 0) { m_saveAt = GetTickCount64() + static_cast<uint64_t>(s.afterSeconds * 1000.0f); Log("recorder: saving in %.0f s", s.afterSeconds); }
        return true;
    }
    const bool saved = Save();
    if (!saved && m_recorder.GetStatus().saving) Log("recorder: a save is still running (a long clip takes a while to write): this press was not needed");
    if (!saved && !m_recorder.GetStatus().saving) {   // said, not silent: a press that saves nothing was the owner's whole afternoon
        Log("recorder: nothing to save yet: no frames have come in. Frame generation on needs no addon; with it off the recorder uses Lossless Scaling's scaling pass (NIS), or an addon that is switched on (Neural Rendering, an upscaler, EchoFG)");
        if (s.beep) SoundNothing();
    }
    return saved;
}

void Service::Poll() {
    const uint64_t now = GetTickCount64();
    NoteForeground();
    const uint64_t clipUntil = m_clipUntil.load();
    if (clipUntil && now >= clipUntil) {   // the clip is done: saved, and the recorder goes back off
        { std::lock_guard<std::mutex> rec(m_recMutex); m_recorder.Save(Folder(), GameName(), true); }
        m_clipUntil = 0;
        ApplyToRecorder();
    }
    const uint64_t saveAt = m_saveAt.load();
    if (saveAt && now >= saveAt) { m_saveAt = 0; Save(); }
    const Settings s = Current();
    const bool modifiers = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    const bool down = s.hotkeyOn && modifiers && s.key > 0 && (GetAsyncKeyState(s.key) & 0x8000);
    if (down && !m_wasDown) {
        Log("hotkey: %s", m_on.load() ? "save the recording" : "record a clip");
        if (s.beep) SoundPress();   // at once: the press is known to have arrived, whatever the file takes
        Request();
    }
    m_wasDown = down;
    AfterSave();
}

// ---- after a save: the note beside the file, and the sound

void Service::AfterSave() {
    const uint32_t saves = m_recorder.SaveCount();
    if (saves == m_seenSaves) return;
    m_seenSaves = saves;
    const Settings s = Current();
    nr::Recorder::SaveInfo info;
    if (!m_recorder.LastSave(info)) return;
    if (s.note) WriteNote(info);
    if (m_saved) m_saved(info.path);
    if (s.beep) SoundSaved();
}

void Service::WriteNote(const nr::Recorder::SaveInfo& info) {
    const Settings s = Current();
    SYSTEMTIME t; GetLocalTime(&t);
    char when[40]; snprintf(when, sizeof when, "%04u-%02u-%02uT%02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::string game = GameName(); if (game.empty()) game = "unknown";
    std::string json = "{\n";
    auto add = [&](const char* key, const std::string& value, bool last = false) { json += std::string("  \"") + key + "\": " + value + (last ? "\n" : ",\n"); };
    char num[64];
    add("saved", JsonString(when));
    add("file", JsonString(Utf8(info.path.substr(info.path.find_last_of(L"\\/") + 1))));
    add("game", JsonString(game));
    snprintf(num, sizeof num, "%u", info.frames); add("frames", num);
    snprintf(num, sizeof num, "%.2f", info.seconds); add("seconds", num);
    snprintf(num, sizeof num, "%.1f", info.bytes / 1048576.0); add("size_mb", num);
    snprintf(num, sizeof num, "%ux%u", info.w, info.h); add("size", JsonString(num));
    snprintf(num, sizeof num, "%u", info.format); add("dxgi_format", num);
    add("source", JsonString(SourceName(info.source)));
    snprintf(num, sizeof num, "%llu", static_cast<unsigned long long>(info.missed)); add("frames_left_out", num);
    snprintf(num, sizeof num, "%.1f", s.seconds); add("seconds_kept_setting", num);
    snprintf(num, sizeof num, "%.1f", s.afterSeconds); add("seconds_after_key", num);
    const std::string extra = m_info ? m_info() : std::string();
    if (!extra.empty()) { json += "  " + extra + ",\n"; }
    json += "  \"note\": \"A .lsrec holds the game's frames as Lossless Scaling captured them, before any addon changed them. Replay it with nr_hosttest replay=<file>, or look into it with nr_lsrec.\"\n}\n";
    const std::wstring path = info.path + L".json";
    HANDLE const h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Log("recorder: the note beside the file could not be written"); return; }
    DWORD written = 0; WriteFile(h, json.data(), static_cast<DWORD>(json.size()), &written, nullptr); CloseHandle(h);
}

std::string Service::Summary() const {
    const nr::Recorder::Status st = m_recorder.GetStatus();
    char text[200];
    if (m_clipUntil.load()) { snprintf(text, sizeof text, "Recording a clip..."); return text; }
    if (m_saveAt.load()) { snprintf(text, sizeof text, "Saving after the wait..."); return text; }
    if (!m_on.load()) return std::string();
    if (!st.frames) { snprintf(text, sizeof text, "Recorder on: waiting for frames (scale a game)"); return text; }
    snprintf(text, sizeof text, "Recorder on: %.1f s kept (%ux%u)%s", st.seconds, st.w, st.h, st.saving ? ", saving..." : "");
    return text;
}

void Service::Shutdown() {
    m_on = false; m_clipUntil = 0; m_saveAt = 0;
    { std::lock_guard<std::mutex> lock(m_tapMutex); m_taps.clear(); m_owner = nullptr; }
    std::lock_guard<std::mutex> rec(m_recMutex);
    m_recorder.Forget();
    m_recorder.Shutdown();
}

} // namespace rec
} // namespace eam
