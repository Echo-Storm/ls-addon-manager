// ECHOFG: EchoFG frame generation (prototype), an addon of its own (it began inside the FSR Upscaler's panel, where little of the rest applied to it).
//
// With Lossless Scaling's own frame generation OFF, every frame it presents is held back, and the frames between it and the one before are made
// and presented first, in Lossless Scaling's own swap chain, evenly spaced: the frame rate multiplied by 2 to 9 (src/addon/framegen11.cpp is the presenter,
// src/engine/fg_engine.cpp and own_synth.cpp make the frames: EchoFG's own passes, or AMD's FSR 3.1 frame generation fed our motion estimate).
//
// What it needs of Lossless Scaling: a Scaling Type that writes the swap chain's back buffer from a compute pass (NIS), and its own frame generation off. It does not need
// an upscaler addon, and works beside any of them (or none). The Present hook is the family's shared one (src/addon/present_hook.cpp): addons chain through it.
// Made only in a test build (CMake NR_FRAMEGEN=ON): never in a release until the owner says it is ready.
#include <eam/addon_exports.h>
#include <eam/events.h>
#include <imgui.h>
#include <eam/widgets.h>
#include "addon/framegen11.h"
#include "addon/present_hook.h"
#include "addon/recorder.h"
#include "addon/hdr.h"
#include "addon/lsrec.h"
#include <windows.h>
#include <shlobj.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {

constexpr const char* kId = "ECHOFG";
constexpr const char* kName = "EchoFG frame generation (prototype)";
constexpr const char* kVersion = "0.10.0";

IHost* g_host = nullptr;
std::wstring g_addonDir;
nr::Recorder g_recorder;

// the settings, read from the host's config twice a second (the presenting thread reads them every frame: atomics)
constexpr float kClipSeconds = 10.0f;   // a clip is always the next ten seconds (run at the walls, run while turning), whatever "Seconds kept" says
std::atomic<ULONGLONG> g_clipUntil{ 0 };   // a one-shot clip (the button, or the hotkey with the recorder off): records until this tick, saves, and the recorder goes back off
std::atomic<bool> g_enabled{ true }, g_own{ true }, g_align{ true }, g_guard{ true }, g_recordOn{ false }, g_hotkeys{ true };
std::atomic<int> g_multiplier{ 2 }, g_recordBudgetMb{ 3072 };
std::atomic<float> g_soften{ 0.6f }, g_recordSeconds{ 5.0f };
std::mutex g_textMutex;
std::string g_recordFolder, g_focusExe, g_notice;
std::atomic<uint64_t> g_configAt{ 0 };
std::atomic<bool> g_wasOn{ false };

// The addon's own log, <Lossless Scaling folder>\logs\ECHOFG.log (the last session; the one before is ECHOFG.log.old), as the other addons keep theirs: tools/dev.py logs reads it
FILE* g_log = nullptr; std::mutex g_logMutex;
void OpenLog(const std::wstring& addonDir) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log) return;
    const size_t slash = addonDir.find_last_of(L'\\');   // <LS folder>\addons\ECHOFG -> <LS folder>\addons
    const size_t slash2 = slash == std::wstring::npos ? slash : addonDir.find_last_of(L'\\', slash - 1);
    if (slash2 == std::wstring::npos) return;
    const std::wstring logs = addonDir.substr(0, slash2) + L"\\logs";
    CreateDirectoryW(logs.c_str(), nullptr);
    MoveFileExW((logs + L"\\ECHOFG.log").c_str(), (logs + L"\\ECHOFG.log.old").c_str(), MOVEFILE_REPLACE_EXISTING);
    _wfopen_s(&g_log, (logs + L"\\ECHOFG.log").c_str(), L"w");
}
void Log(const char* fmt, ...) {
    char text[600]; va_list a; va_start(a, fmt); vsnprintf(text, sizeof text, fmt, a); va_end(a);
    if (g_host) g_host->Log(EAM_LOG_INFO, (std::string(kId) + ": " + text).c_str());
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log) { SYSTEMTIME st; GetLocalTime(&st); fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, text); fflush(g_log); }
}

std::wstring FolderOf(HMODULE module) {
    wchar_t path[MAX_PATH]; GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring p = path; const size_t slash = p.find_last_of(L'\\');
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}
std::wstring Wide(const std::string& s) {
    std::wstring w(s.size(), L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    w.resize(n > 0 ? n : 0);
    return w;
}

void ReadConfig() {
    const uint64_t now = GetTickCount64();
    if (g_configAt.load() && now - g_configAt.load() < 250) return;
    g_configAt = now;
    auto flag = [](const char* key, bool def) { return std::string(g_host->GetConfig(kId, key, def ? "1" : "0")) == "1"; };
    g_enabled = flag("enabled", true);   // (the manager's switch for the addon is the real one: this only pauses)
    g_own = flag("own", true); g_align = flag("align", true); g_guard = flag("guard", true);
    g_recordOn = flag("recordOn", false); g_hotkeys = flag("hotkeys", true);
    g_multiplier = std::clamp(atoi(g_host->GetConfig(kId, "multiplier", "2")), 2, 9);
    g_soften = std::clamp(static_cast<float>(atof(g_host->GetConfig(kId, "soften", "0.6"))), 0.0f, 1.0f);
    g_recordSeconds = std::clamp(static_cast<float>(atof(g_host->GetConfig(kId, "recordSeconds", "5"))), 1.0f, 60.0f);
    g_recordBudgetMb = std::clamp(atoi(g_host->GetConfig(kId, "recordBudgetMb", "3072")), 256, 65536);
    std::lock_guard<std::mutex> lock(g_textMutex);
    g_recordFolder = g_host->GetConfig(kId, "recordFolder", "");
}
void Set(const char* key, const std::string& value) { g_host->SetConfig(kId, key, value.c_str()); g_host->SaveConfig(); g_configAt = 0; }

std::wstring RecordFolder() {
    std::string chosen; { std::lock_guard<std::mutex> lock(g_textMutex); chosen = g_recordFolder; }
    if (!chosen.empty()) { std::wstring w = Wide(chosen); for (wchar_t& ch : w) if (ch == L'/') ch = L'\\'; return w; }
    PWSTR videos = nullptr; std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos)) && videos) folder = videos;
    CoTaskMemFree(videos);
    if (folder.find(L"\\OneDrive") != std::wstring::npos) {   // (a recording is gigabytes: not into a folder that uploads)
        PWSTR profile = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile)) && profile) folder = std::wstring(profile) + L"\\Videos";
        CoTaskMemFree(profile);
    }
    return (folder.empty() ? g_addonDir : folder) + L"\\Lossless Scaling";
}

// The program in focus (the game), without .exe; empty when it is this process (Lossless Scaling) or unknown.
std::string FocusGame() {
    const HWND window = GetForegroundWindow(); DWORD pid = 0;
    if (!window) return {};
    GetWindowThreadProcessId(window, &pid);
    if (!pid || pid == GetCurrentProcessId()) return {};
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH]; DWORD size = MAX_PATH; std::string exe;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        const wchar_t* name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
        char utf8[260]; const int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof utf8, nullptr, nullptr);
        if (n > 0) exe = utf8;
    }
    CloseHandle(process);
    if (exe.size() > 4 && _stricmp(exe.c_str() + exe.size() - 4, ".exe") == 0) exe.resize(exe.size() - 4);
    for (char& c : exe) if (c == ' ') c = '_';
    return exe;
}

void SaveRecording() {
    if (!g_recordOn) {   // the recorder is off: record the next seconds, save them by themselves, and be off again (nothing is left running)
        if (g_clipUntil.load()) return;
        g_clipUntil = GetTickCount64() + static_cast<ULONGLONG>(kClipSeconds * 1000.0f) + 500;
        Log("recorder: recording the next %.0f s, then saving", kClipSeconds); g_host->SetStatus(kId, "Recording a clip...", 3);
        return;
    }
    std::string game; { std::lock_guard<std::mutex> lock(g_textMutex); game = g_focusExe; }
    if (game.empty()) game = "EchoFG";
    if (!g_recorder.Save(RecordFolder(), game)) Log("recorder: nothing saved (%s)", g_recorder.GetStatus().saving ? "a save is running" : "nothing recorded yet");
}

DXGI_FORMAT ViewFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

// Ctrl+Shift+F1 saves the recording (rising edge, polled on the presenting thread)
void PollHotkey() {
    static bool wasDown = false;
    const bool down = g_hotkeys && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState(VK_F1) & 0x8000);
    if (down && !wasDown) { Log("hotkey: save the recording"); SaveRecording(); }
    wasDown = down;
}

// Every present of the process (Lossless Scaling's output chain, and the manager's own window): the presenter takes only the one whose back buffer Lossless Scaling's compute pass writes.
void OnPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!g_host || !sc) return;
    ReadConfig();
    if (!g_enabled) {
        if (g_wasOn.exchange(false)) nr::framegen::Shutdown();   // switched off: its engine, device and textures go
        return;
    }
    { const std::string game = FocusGame(); if (!game.empty()) { std::lock_guard<std::mutex> lock(g_textMutex); g_focusExe = game; } }
    PollHotkey();
    static const bool runtimeSet = (nr::framegen::SetRuntime(g_addonDir + L"\\fsr\\amd_fidelityfx_dx12.dll"), true);   // the shipped FSR 3.1
    (void)runtimeSet;
    float white = 80.0f; uint32_t encoding = 0;
    { ID3D11Texture2D* back = nullptr;   // what its frames hold (scRGB or HDR10 on an HDR display), for measuring their motion in their SDR view
      if (SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
          D3D11_TEXTURE2D_DESC d; back->GetDesc(&d);
          ID3D11Device* dev = nullptr; back->GetDevice(&dev);
          const nr::DisplayHdr display = nr::QueryDisplayHdr(sc, dev);
          if (dev) dev->Release();
          encoding = static_cast<uint32_t>(nr::EncodingOf(ViewFormat(d.Format), 0, display.hdr)); white = display.whiteNits;
          back->Release();
      } }
    nr::framegen::SetOwn(g_own); nr::framegen::SetMultiplier(g_multiplier); nr::framegen::SetSoften(g_soften); nr::framegen::SetAlign(g_align); nr::framegen::SetGuard(g_guard);
    static bool recorderLogSet = (g_recorder.SetLog([](const char* m) { Log("%s", m); }), true);
    (void)recorderLogSet;
    const ULONGLONG clipUntil = g_clipUntil.load();
    const bool clipping = clipUntil != 0 && !g_recordOn;
    if (clipping && GetTickCount64() >= clipUntil) {   // the clip is done: saved, and the recorder goes back off
        std::string game; { std::lock_guard<std::mutex> lock(g_textMutex); game = g_focusExe; }
        if (game.empty()) game = "EchoFG";
        if (!g_recorder.Save(RecordFolder(), game, true)) Log("recorder: the clip was not saved (%s)", g_recorder.GetStatus().saving ? "a save is running" : "nothing recorded");
        g_clipUntil = 0;
    }
    const bool recorderOn = g_recordOn || (g_clipUntil.load() != 0);
    const bool clipRunning = g_clipUntil.load() != 0 && !g_recordOn;   // (a clip keeps all ten seconds, in as much memory as they take: 4K HDR is big)
    g_recorder.Configure(recorderOn, clipRunning ? std::max(g_recordSeconds.load(), kClipSeconds) : g_recordSeconds.load(), clipRunning ? std::max<uint32_t>(static_cast<uint32_t>(g_recordBudgetMb.load()), 8192u) : static_cast<uint32_t>(g_recordBudgetMb.load()));
    nr::framegen::ShownFn shown;   // every frame presented, the frames between and the real ones, tagged; the middle 1920x1080 only (4K HDR frames in full are more than the card can copy)
    if (recorderOn) shown = [](ID3D11DeviceContext* ctx, ID3D11Texture2D* frame, bool made) { g_recorder.Offer(ctx, frame, nr::lsrec::kPresented, 0, made ? nr::lsrec::kMadeBetween : nr::lsrec::kReal, 1920, 1080); };
    g_wasOn = true;
    nr::framegen::BeforeRealPresent(sc, sync, flags, encoding, white, [](const char* m) { Log("%s", m); }, shown);
}

// Lossless Scaling makes a new device when scaling starts: the hook is installed on it (once: the table is the DXGI runtime's own), what was made on an old one goes
void OnDevice(uint32_t, const void*, uint32_t, void*) {
    nr::framegen::Reset();
    g_recorder.Forget();
    if (!g_host) return;
    if (auto* dev = static_cast<ID3D11Device*>(g_host->GetD3D11Device())) {
        if (!PresentHook::Installed() && !PresentHook::Install(dev, OnPresent, [](const char* m) { Log("%s", m); })) Log("could not hook dxgi Present");
    }
}

}   // namespace

// A step of the start run under a guard that says what it hit (the code and where), so a crash in the start is a line in the log and not only "crashed during init"
int Filter(EXCEPTION_POINTERS* info, unsigned long* code, void** where) { *code = info->ExceptionRecord->ExceptionCode; *where = info->ExceptionRecord->ExceptionAddress; return EXCEPTION_EXECUTE_HANDLER; }
bool Guarded(const char* step, void (*fn)()) {
    unsigned long code = 0; void* where = nullptr;
    __try { fn(); return true; }
    __except (Filter(GetExceptionInformation(), &code, &where)) {}
    HMODULE module = nullptr; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(where), &module);
    wchar_t name[MAX_PATH] = L"?"; if (module) GetModuleFileNameW(module, name, MAX_PATH);
    const wchar_t* base = wcsrchr(name, L'\\'); base = base ? base + 1 : name;
    Log("the start failed in %s: exception 0x%08lX at %ls+0x%llX", step, code, base, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(where) - reinterpret_cast<uintptr_t>(module)));
    return false;
}
void SubscribeAll() {
    g_host->SubscribeEvent(EAM_EVENT_D3D11_DEVICE_READY, OnDevice, nullptr);
    g_host->SubscribeEvent(EAM_EVENT_D3D11_DEVICE_CHANGED, OnDevice, nullptr);
}

EAM_EXPORT void AddonInitialize(IHost* host, ImGuiContext* ctx, void* allocFunc, void* freeFunc, void* userData) {
    ImGui::SetCurrentContext(ctx);
    ImGui::SetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(allocFunc), reinterpret_cast<ImGuiMemFreeFunc>(freeFunc), userData);
    eam::ui::InitAddonImGui();
    g_host = host;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&AddonInitialize), &self);
    g_addonDir = FolderOf(self);
    OpenLog(g_addonDir);
    Log("%s initialised (host version 0x%06x), addon dir %ls", kId, host->GetHostVersion(), g_addonDir.c_str());   // (the line tools/dev.py logs takes for the start of a session)
    g_configAt = 0;
    Guarded("reading the settings", [] { ReadConfig(); });
    Guarded("subscribing to the device events", SubscribeAll);
    // (no device is taken from the host here: the newest one it has seen may be gone already, Lossless Scaling makes and drops them as scaling starts and stops. The device comes with the event, when scaling starts: start scaling after switching the addon on.)
    Log("%s %s initialised (enabled %d, %s, x%d)", kName, kVersion, g_enabled ? 1 : 0, g_own ? "EchoFG" : "FSR 3.1", g_multiplier.load());
    host->SetStatus(kId, g_enabled ? "On: waits for Lossless Scaling's frames" : "Paused (tick EchoFG is on)", g_enabled ? 1 : 0);
}

EAM_EXPORT void AddonShutdown() {
    if (g_host) { g_host->UnsubscribeEvent(EAM_EVENT_D3D11_DEVICE_READY, OnDevice); g_host->UnsubscribeEvent(EAM_EVENT_D3D11_DEVICE_CHANGED, OnDevice); }
    PresentHook::Uninstall();   // (the hook's functions stay in the table and pass straight through)
    nr::framegen::Shutdown();
    g_recorder.Forget(); g_recorder.Shutdown();
    g_host = nullptr;
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_log) { fclose(g_log); g_log = nullptr; }
}

EAM_EXPORT void AddonRenderSettings() {
    ReadConfig();
    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.25f, 1.0f), "Turn this on BEFORE you start the game (before scaling starts).");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("EchoFG gets what it needs from Lossless Scaling at the moment scaling starts, so it only works from the next time scaling starts. "
                                                  "If scaling is already running, stop it and start it again. Switching it off or on while scaling runs changes nothing until then.");
    ImGui::Spacing();
    ImGui::TextWrapped("EchoFG multiplies the frame rate: with Lossless Scaling's own frame generation OFF, every frame is held back a little, and 1 to 8 frames made between it and the one before are shown first, evenly spaced in time. "
                       "It needs Lossless Scaling scaling with NIS (the game in a window smaller than the screen, or fullscreen scaling), so that the frames can be written. It works beside any upscaler addon, or none.");
    ImGui::Spacing();
    bool on = g_enabled;
    if (ImGui::Checkbox("EchoFG is on (untick to pause it without switching the addon off)", &on)) Set("enabled", on ? "1" : "0");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turn Lossless Scaling's own frame generation OFF first: with both on, the frames are doubled twice. A prototype: the real frames are shown a little later than without it (about one real frame).");
    int engine = g_own ? 0 : 1;
    ImGui::SetNextItemWidth(260);
    if (ImGui::Combo("Makes the frames", &engine, "EchoFG (our own)\0FSR 3.1 (AMD's, fed our motion)\0")) Set("own", engine == 0 ? "1" : "0");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("EchoFG: this project's own passes (motion splatted to the time of each frame, holes filled, doubtful places softened). It can make several frames per real one. FSR 3.1: AMD's frame generation, fed our motion estimate; one frame between each two real ones.");
    if (g_own) {
        int m = g_multiplier;
        ImGui::SetNextItemWidth(260);
        if (ImGui::SliderInt("Frames for each real frame", &m, 2, 9, "x%d")) Set("multiplier", std::to_string(m));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("x2 (the default): one frame made between each two real ones. x3 and up: that many frames per real frame, spaced evenly; each costs the card about 1.2 ms more at 2560x1440 (about 2.7 ms at 4K).");
        float s = g_soften;
        ImGui::SetNextItemWidth(260);
        if (ImGui::SliderFloat("Soften where the motion is doubtful", &s, 0.0f, 1.0f, "%.2f")) { char t[16]; snprintf(t, sizeof t, "%.2f", s); Set("soften", t); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Where EchoFG cannot tell where things went (a very fast turn at a low frame rate, something uncovered) the frame leans toward the plain mix of the two real frames, as far as this says: a faint double image instead of a smear.");
    } else {
        bool guard = g_guard;
        if (ImGui::Checkbox("Keep the character clean in turns", &guard)) Set("guard", guard ? "1" : "0");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A guard against FSR pasting background over a character in a fast turn.");
    }
    bool align = g_align;
    if (ImGui::Checkbox("Put the presents one to a refresh of the display", &align)) Set("align", align ? "1" : "0");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("On (the default): each frame is shown in a refresh of its own, so none is replaced by the next within the same refresh. Costs up to half a refresh of delay. Has no effect when the display's refresh is not steady (a variable refresh rate, G-Sync); the log says whether it is in use.");
    ImGui::Spacing();
    {   // what it is doing
        const nr::framegen::Stats st = nr::framegen::GetStats();
        if (!g_enabled) ImGui::TextDisabled("Off.");
        else if (st.real == 0) ImGui::TextDisabled("On: waiting for Lossless Scaling's frames (scaling with NIS, frame generation off).");
        else ImGui::TextWrapped("On: %llu real frames %.1f ms apart, %llu made between by %s; the first frame between is presented in %.2f ms.",
                                static_cast<unsigned long long>(st.real), st.realIntervalMs, static_cast<unsigned long long>(st.generated), st.engine.empty() ? "the blend" : st.engine.c_str(), st.generatedMs);
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Recording");
    bool rec = g_recordOn;
    if (ImGui::Checkbox("Keep the last few seconds ready to save", &rec)) Set("recordOn", rec ? "1" : "0");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keeps the frames as they are SHOWN (the frames between and the real ones, tagged), the middle 1920x1080 of each, losslessly compressed in memory. Ctrl+Shift+F1 (or the button) saves them as a .lsrec file you can send for a look.");
    float secs = g_recordSeconds;
    ImGui::SetNextItemWidth(260);
    if (ImGui::SliderFloat("Seconds kept", &secs, 1.0f, 30.0f, "%.0f s")) { char t[16]; snprintf(t, sizeof t, "%.0f", secs); Set("recordSeconds", t); }
    const nr::Recorder::Status rs = g_recorder.GetStatus();
    if (ImGui::Button(rs.saving ? "Saving..." : g_clipUntil.load() ? "Recording..." : g_recordOn ? "Save the last seconds" : "Record a clip")) SaveRecording();
    if (ImGui::IsItemHovered() && !g_recordOn) ImGui::SetTooltip("The recorder is off: this records the next 10 seconds, saves them as a .lsrec by itself, and goes off again. Ctrl+Shift+F1 does the same.");
    ImGui::SameLine();
    ImGui::TextDisabled("%u frames kept (%.1f s)%s%s", rs.frames, rs.seconds, rs.missed ? ", some left out: the card could not keep up" : "", g_recordOn ? "" : " (off: the button records a clip and saves it)");
    { bool ok = true; const std::string last = g_recorder.LastResult(ok); if (!last.empty()) ImGui::TextColored(ok ? ImVec4(0.7f, 0.9f, 0.5f, 1.0f) : ImVec4(0.95f, 0.65f, 0.2f, 1.0f), "%s", last.c_str()); }
    ImGui::Spacing();
    ImGui::TextDisabled("Log: <Lossless Scaling folder>\\logs (the manager's log has this addon's lines). Every ten seconds it says what the display did with the presents.");
}

EAM_EXPORT uint32_t GetAddonCapabilities() { return EAM_CAP_HAS_SETTINGS | EAM_CAP_D3D11_DEVICE_ACCESS; }
EAM_EXPORT const char* GetAddonName() { return kName; }
EAM_EXPORT const char* GetAddonVersion() { return kVersion; }
EAM_EXPORT const char* GetAddonAuthor() { return "Echo-Storm"; }
EAM_EXPORT const char* GetAddonDescription() { return "A prototype. Multiplies the frame rate with frames made between the real ones (EchoFG's own passes, or AMD's FSR 3.1 frame generation fed our motion estimate), shown in Lossless Scaling's own window. Turn Lossless Scaling's own frame generation off."; }
