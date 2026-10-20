#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "recorder_feature.h"
#include "recorder_service.h"
#include "../config/config_manager.h"
#include "../log/logger.h"
#include "../../sdk/include/eam/version.h"
#include "imgui.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>

namespace eam {
namespace features {
namespace recorder {

namespace {

ConfigManager& Cfg() { return ConfigManager::Instance(); }

Backends g_backends;
std::unique_ptr<rec::Service> g_service;
std::atomic<rec::Service*> g_live{ nullptr };   // what the hooks use (null while off)
std::thread g_poll;
std::atomic<bool> g_stop{ false };

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
std::string JsonText(const std::string& s) {
    std::string out = "\"";
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) out += ' ';
        else out += static_cast<char>(c);
    }
    return out + "\"";
}

// ---- the settings (config.json, section "Recorder")

rec::Settings Load() {
    rec::Settings s;
    s.on = Cfg().Get(kId, "keep", "1") == "1";   // the rolling buffer; off: the key records the next ten seconds
    s.shown = Cfg().Get(kId, "shown", "0") == "1";
    s.seconds = std::clamp(static_cast<float>(std::atof(Cfg().Get(kId, "seconds", "15").c_str())), 1.0f, 60.0f);
    s.budgetMb = std::clamp(std::atoi(Cfg().Get(kId, "budget_mb", "8192").c_str()), 256, 65536);
    s.folder = Wide(Cfg().Get(kId, "folder", ""));
    s.hotkeyOn = Cfg().Get(kId, "hotkey", "1") == "1";
    s.key = std::atoi(Cfg().Get(kId, "key", "112").c_str());   // 112 = VK_F1
    s.afterSeconds = std::clamp(static_cast<float>(std::atof(Cfg().Get(kId, "after_seconds", "0").c_str())), 0.0f, 60.0f);
    s.compact = Cfg().Get(kId, "compact", "1") == "1";
    s.beep = Cfg().Get(kId, "beep", "1") == "1";
    s.note = Cfg().Get(kId, "note", "1") == "1";
    s.saveAfter = std::max(0, std::atoi(Cfg().Get(kId, "test_save_after", "0").c_str()));   // tests only
    return s;
}

void Store(const rec::Settings& s) {
    Cfg().Set(kId, "keep", s.on ? "1" : "0");
    Cfg().Set(kId, "shown", s.shown ? "1" : "0");
    Cfg().Set(kId, "seconds", std::to_string(static_cast<int>(s.seconds + 0.5f)));
    Cfg().Set(kId, "budget_mb", std::to_string(s.budgetMb));
    Cfg().Set(kId, "folder", Utf8(s.folder));
    Cfg().Set(kId, "hotkey", s.hotkeyOn ? "1" : "0");
    Cfg().Set(kId, "key", std::to_string(s.key));
    Cfg().Set(kId, "after_seconds", std::to_string(static_cast<int>(s.afterSeconds + 0.5f)));
    Cfg().Set(kId, "compact", s.compact ? "1" : "0");
    Cfg().Set(kId, "beep", s.beep ? "1" : "0");
    Cfg().Set(kId, "note", s.note ? "1" : "0");
    Cfg().Save();
}

// ---- what the hooks do

void Observe(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z) {
    if (rec::Service* const s = g_live.load(std::memory_order_acquire)) s->OnDispatch(ctx, x, y, z);
}
uint32_t FlagsFn() {
    rec::Service* const s = g_live.load(std::memory_order_acquire);
    return s ? s->Flags() : 0u;
}
void OfferFn(void* ctx, void* texture, uint32_t source, uint32_t content, uint32_t tag, uint32_t cropW, uint32_t cropH) {
    if (rec::Service* const s = g_live.load(std::memory_order_acquire))
        s->Offer(static_cast<ID3D11DeviceContext*>(ctx), static_cast<ID3D11Texture2D*>(texture), source, content, tag, cropW, cropH);
}

// What the note beside a saved file says about the manager and the addons: which are on and the few settings that decide how a picture comes out.
std::string Info() {
    static const char* const ids[] = { "DLSS5NR01", "DLSS4DLAA", "FSR3UPSC", "XESSUPSC", "ECHOFG", "VSRUPSC" };
    static const char* const keys[] = { "modelPreset", "passes", "workingScale", "autoQuality", "intensity", "style", "dlaaPreset", "sharpen", "multiplier", "own" };
    std::string out = std::string("\"manager_version\": ") + JsonText(EAM_VERSION_STRING) + ",\n  \"addons\": {";
    bool firstAddon = true;
    for (const char* id : ids) {
        out += firstAddon ? "\n" : ",\n"; firstAddon = false;
        out += std::string("    ") + JsonText(id) + ": { \"enabled\": " + (Cfg().IsAddonEnabled(id, false) ? "true" : "false");
        for (const char* key : keys) {
            const std::string v = Cfg().Get(id, key, "");
            if (!v.empty()) out += std::string(", ") + JsonText(key) + ": " + JsonText(v);
        }
        out += " }";
    }
    out += "\n  }";
    return out;
}

int SaveFn(const char* name, bool clear) {
    rec::Service* const s = g_live.load(std::memory_order_acquire);
    return s ? s->SaveNamed(name ? name : "", clear) : 0;
}

void PollLoop() {
    while (!g_stop.load()) {
        if (rec::Service* const s = g_live.load(std::memory_order_acquire)) s->Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

void ApplyFromConfig() { if (g_service) g_service->Configure(Load()); }

} // namespace

void SetBackends(const Backends& backends) { g_backends = backends; }

void Start() {
    if (g_service) return;
    g_service = std::make_unique<rec::Service>();
    g_service->SetLog([](const char* m) { LOG_INFO("Recorder", "%s", m); });
    g_service->SetInfoProvider(&Info);
    ApplyFromConfig();
    g_stop = false;
    g_poll = std::thread(PollLoop);
    g_live.store(g_service.get(), std::memory_order_release);
    if (g_backends.publishSaved) g_service->SetSavedCallback([](const std::wstring& path) { g_backends.publishSaved(path.c_str()); });
    if (g_backends.setHostHooks) g_backends.setHostHooks(&FlagsFn, &OfferFn, &SaveFn);
    if (g_backends.setDispatchObserver) g_backends.setDispatchObserver(&Observe);
    LOG_INFO("Recorder", "on");
}

void Stop() {
    if (!g_service) return;
    if (g_backends.setDispatchObserver) g_backends.setDispatchObserver(nullptr);
    if (g_backends.setHostHooks) g_backends.setHostHooks(nullptr, nullptr, nullptr);
    g_live.store(nullptr, std::memory_order_release);
    g_stop = true;
    if (g_poll.joinable()) g_poll.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));   // a dispatch that had the pointer finishes
    g_service->Shutdown();
    g_service.reset();
    LOG_INFO("Recorder", "off");
}

std::string Status() { return g_service ? g_service->Summary() : std::string(); }

void RenderOptions() {
    if (!g_service) return;
    rec::Settings s = g_service->Current();
    bool changed = false;

    if (ImGui::Checkbox("Keep the last few seconds ready to save", &s.on)) changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On: the recorder keeps the last seconds in memory all the time, and the hotkey saves them at once.\n"
                          "Off: nothing is kept; the hotkey records the next ten seconds, saves them and stops (it holds the memory only for that).");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    float seconds = s.seconds; if (ImGui::SliderFloat("Seconds kept", &seconds, 1.0f, 60.0f, "%.0f s")) { s.seconds = seconds; changed = true; }
    {
        int mode = s.compact ? 0 : 1; const char* modes[] = { "Compact (8-bit picture, small)", "Exact (as captured, huge for HDR)" };
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::Combo("Frames", &mode, modes, 2)) { s.compact = mode == 0; changed = true; }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Compact: HDR frames are made into an 8-bit SDR picture (what you see; highlights above the SDR white clip) on the card first: about a sixth of the size, a 10 s clip about 1 GB, saved in seconds.\n"
                              "Exact: every frame as Lossless Scaling captured it, half-float HDR included: about 0.85 GB a second at 2560x1440, a minute to write. For a bug that depends on the HDR values.\n"
                              "8-bit frames are kept as they are in both. Changing it starts the recording again.");
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    int budget = s.budgetMb; if (ImGui::SliderInt("Memory for it", &budget, 512, 32768, "%d MB")) { s.budgetMb = budget; changed = true; }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The older frames go first when this is full. A second of 4K HDR can take 200 MB or more, even compressed: raise it for long clips at 4K.");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    float after = s.afterSeconds; if (ImGui::SliderFloat("Keep going after the key", &after, 0.0f, 30.0f, after < 0.5f ? "save at once" : "%.0f s")) { s.afterSeconds = after; changed = true; }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("After the hotkey, keep recording this many seconds and then save: the file holds the seconds before the key and these after it.");
    if (ImGui::Checkbox("Record what is shown", &s.shown)) changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Record the picture as Lossless Scaling shows it (the addons offer their presented frames) instead of the frames it captured from the game.\n"
                          "Needs an addon that offers them (Neural Rendering, the upscalers, EchoFG) to be switched on. For looking at flicker and trailing in what you see.");

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Hotkey");
    ImGui::SameLine();
    if (ImGui::Checkbox("##hotkey_on", &s.hotkeyOn)) changed = true;
    ImGui::SameLine();
    ImGui::TextDisabled("Ctrl+Shift +");
    ImGui::SameLine();
    char name[16]; snprintf(name, sizeof name, "F%d", s.key - 0x70 + 1);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
    if (ImGui::BeginCombo("##recorder_key", name)) {
        for (int i = 0; i < 12; ++i) {
            char n[8]; snprintf(n, sizeof n, "F%d", i + 1);
            if (ImGui::Selectable(n, s.key == 0x70 + i)) { s.key = 0x70 + i; changed = true; }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Press it while the game has the focus. OBS' replay-buffer key can be the same one: one press then saves both.");

    if (ImGui::Checkbox("Sounds (a tick at the key, a rising pair when saved, a low tone if there was nothing)", &s.beep)) changed = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Test the sound")) g_service->TestSound();
    ImGui::SameLine();
    if (ImGui::Checkbox("Write a note beside it", &s.note)) changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("<file>.lsrec.json: the game, the picture, which addons were on and their main settings, so a clip says how it was made.");

    static char folder[512] = {};
    static bool folderRead = false;
    if (!folderRead) { snprintf(folder, sizeof folder, "%s", Utf8(s.folder).c_str()); folderRead = true; }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 30.0f);
    ImGui::InputTextWithHint("##recorder_folder", "Folder (empty: Videos\\Lossless Scaling)", folder, sizeof folder);
    if (ImGui::IsItemDeactivatedAfterEdit()) { s.folder = Wide(folder); changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("Open")) {
        std::wstring dir = s.folder;
        if (dir.empty()) { PWSTR v = nullptr; if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &v)) && v) { dir = std::wstring(v) + L"\\Lossless Scaling"; CoTaskMemFree(v); } }
        CreateDirectoryW(dir.c_str(), nullptr);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    if (ImGui::Button(g_service->Clipping() ? "Recording a clip..." : g_service->Pending() ? "Saving soon..." : s.on ? "Save the last seconds" : "Record a clip")) g_service->Request();
    ImGui::SameLine();
    const std::string summary = g_service->Summary();
    ImGui::TextDisabled("%s", summary.empty() ? "idle" : summary.c_str());
    bool ok = true; const std::string last = g_service->LastResult(ok);
    if (!last.empty()) ImGui::TextColored(ok ? ImVec4(0.7f, 0.9f, 0.5f, 1.0f) : ImVec4(0.95f, 0.65f, 0.2f, 1.0f), "%s", last.c_str());
    ImGui::TextDisabled("It finds Lossless Scaling's captured frames by itself, with frame generation on or off (off: the frame its scaling pass reads); no addon has to be switched on.");

    if (changed) { Store(s); g_service->Configure(s); }
}

} // namespace recorder
} // namespace features
} // namespace eam
