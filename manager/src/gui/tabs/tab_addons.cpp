#include "eam/widgets.h"
#include "tab_addons.h"
#include "../gui_scale.h"
#include "../gui_style.h"
#include "../../addon/addon_manager.h"
#include "../../config/config_manager.h"
#include "../widgets/addon_card.h"
#include "../widgets/toast.h"
#include "../widgets/empty_state.h"
#include "../widgets/runtime_list.h"
#include "../widgets/tooltip.h"
#include "../widgets/toggle_switch.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")

namespace eam {

// ---- installing an addon ------------------------------------------------------------------------------------------
static std::wstring s_installSource;        // what the user picked, waiting for confirmation
static bool s_askInstall = false;
static std::string s_removeId;              // the addon waiting for the remove confirmation
static bool s_askRemove = false;

void RequestInstallFromPath(const std::wstring& path) { s_installSource = path; s_askInstall = true; }

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    std::string s((size_t)WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], (int)s.size(), nullptr, nullptr);
    return s;
}

// The system's own open dialog, for a folder or for a .zip / .dll. Only ever shown when the user clicked for it.
static bool PickPath(bool folder, std::wstring& out) {
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0; dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
        if (!folder) { COMDLG_FILTERSPEC f[] = { { L"Addon (.zip or .dll)", L"*.zip;*.dll" }, { L"All files", L"*.*" } }; dlg->SetFileTypes(2, f); }
        dlg->SetTitle(folder ? L"Choose an addon folder" : L"Choose an addon zip or DLL");
        if (SUCCEEDED(dlg->Show(GetActiveWindow()))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); ok = true; }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(ci)) CoUninitialize();
    return ok;
}

static char s_searchBuffer[256] = {};


// Selection is by addon id so it survives reordering, and is remembered across sessions.
static std::string s_selectedId;
static bool s_selectionLoaded = false;
static std::string s_tabsOpenedFor; // addon the detail tab bar last picked a default tab for

// Inline config editor state (one file at a time, loaded when the Config tab is shown).
static std::wstring s_cfgPath;
static std::vector<char> s_cfgBuf;
static bool s_cfgDirty = false;
static constexpr size_t kCfgBufSize = 1024 * 1024;

static bool s_cfgTooBig = false;   // file does not fit the edit buffer: editing it would truncate it

static void LoadConfigFile(const std::wstring& path) {
    s_cfgPath = path;
    s_cfgBuf.assign(kCfgBufSize, 0);
    s_cfgDirty = false;
    s_cfgTooBig = false;
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return;
    std::stringstream buf;
    buf << file.rdbuf();
    const std::string content = buf.str();
    if (content.size() >= kCfgBufSize) { s_cfgTooBig = true; return; }
    memcpy(s_cfgBuf.data(), content.data(), content.size());
}

static void SaveConfigFile() {
    if (s_cfgPath.empty() || s_cfgTooBig) return;
    // Write a sibling file and swap it in so an interrupted save cannot leave half a config.
    const std::wstring tmp = s_cfgPath + L".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        file << s_cfgBuf.data();
        file.flush();
        if (!file) {
            widgets::ToastShow("Could not write the config file", widgets::ToastType::Error);
            return;
        }
    }
    if (!MoveFileExW(tmp.c_str(), s_cfgPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        widgets::ToastShow("Could not replace the config file", widgets::ToastType::Error);
        return;
    }
    s_cfgDirty = false;
    widgets::ToastShow("Config saved", widgets::ToastType::Success);
}

// Lower case for the search box. Not ::tolower on plain chars: that is undefined for non-ASCII text (an addon named with an accented letter).
static std::string LowerCase(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// The search box: every word typed must appear somewhere in the addon's name, author, id, description or tags (any case), so
// "fsr upscaler" or "nvidia dlss" find what people expect. Words are separated by spaces.
static bool MatchesFilter(const AddonInfo& addon, const char* filter) {
    if (!filter) return true;
    std::string haystack = addon.GetDisplayName() + '\n' + addon.GetDisplayAuthor() + '\n' + addon.id + '\n' + addon.manifest.description;
    for (const auto& tag : addon.manifest.tags) haystack += '\n' + tag;
    haystack = LowerCase(haystack);
    const std::string words = LowerCase(filter);
    for (size_t at = 0; at < words.size();) {
        const size_t from = words.find_first_not_of(' ', at);
        if (from == std::string::npos) break;
        const size_t to = (std::min)(words.find(' ', from), words.size());
        if (haystack.find(words.substr(from, to - from)) == std::string::npos) return false;
        at = to;
    }
    return true;
}

static void SelectAddon(const AddonInfo& addon) {
    if (s_selectedId == addon.id) return;
    s_selectedId = addon.id;
    auto& cfg = ConfigManager::Instance();
    cfg.GlobalSet("ui", "selected_addon", addon.id);
    cfg.Save();
}

// The card and the detail pane both flip an addon through here, so they behave identically.
static void ApplyToggle(AddonManager* manager, int index, bool enable) {
    const std::vector<std::string> off = manager->ToggleAddon(index, enable);
    std::string also;   // the addons it cannot run beside, turned off
    for (const std::string& name : off) also += (also.empty() ? "" : ", ") + name;
    if (manager->GetAddons()[index].RequiresRestart()) {
        widgets::ToastShow(enable ? "Restart Lossless Scaling to enable this addon"
                                  : "Restart Lossless Scaling to disable this addon",
                           widgets::ToastType::Warning, 5.0f);
    } else if (enable && !manager->GetAddons()[index].IsLoaded()) {
        const std::string& why = manager->GetAddons()[index].errorMessage;
        widgets::ToastShow(why.empty() ? "Addon enabled, but it could not be loaded" : why,
                           widgets::ToastType::Error, 6.0f);
    } else if (!also.empty()) {
        widgets::ToastShow("Addon enabled. Turned off " + also + ": the two cannot run at the same time.", widgets::ToastType::Success, 5.0f);
    } else {
        widgets::ToastShow(enable ? "Addon enabled" : "Addon disabled",
                           enable ? widgets::ToastType::Success : widgets::ToastType::Info);
    }
}

static void StatusText(const char* text, ImVec4 color) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

static void RenderSettingsPane(AddonManager* manager, const AddonInfo& addon, int index) {
    ImGui::Dummy(ImVec2(0, S(4)));
    if (!addon.enabled) {
        ImGui::TextDisabled("This addon is disabled. Turn it on (top right) to configure it.");
        return;
    }
    if (!addon.IsLoaded()) {
        if (!addon.errorMessage.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.816f, 0.502f, 0.502f, 1));
            ImGui::TextWrapped("%s", addon.errorMessage.c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, S(4)));
        }
        if (addon.RequiresRestart()) {
            ImGui::TextWrapped("Restart Lossless Scaling to load this addon.");
        } else {
            ImGui::TextWrapped("This addon is enabled but not loaded.");
            if (eam::ui::Button("Load now", eam::ui::icons::kPlay, eam::ui::ButtonKind::Primary)) {
                manager->LoadAddonNow(index);
                widgets::ToastShow(manager->GetAddons()[index].IsLoaded() ? "Addon loaded" : "Could not load the addon",
                                   manager->GetAddons()[index].IsLoaded() ? widgets::ToastType::Success
                                                                          : widgets::ToastType::Error);
            }
        }
        return;
    }
    // The addon draws its own controls into this child; long status lines can scroll sideways.
    ImGui::BeginChild("##addon_settings", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    const float y0 = ImGui::GetCursorPosY();
    manager->RenderAddonSettings(index);
    if (ImGui::GetCursorPosY() == y0)
        ImGui::TextWrapped("This addon did not draw any controls. Some addons only show settings while a game is "
                           "being scaled; if it never does, check the Logs tab for errors.");
    ImGui::EndChild();
}

static void RenderOverviewPane(AddonManager* manager, const AddonInfo& addon) {
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::BeginChild("##addon_overview", ImVec2(0, 0), false);

    if (!addon.manifest.description.empty())
        ImGui::TextWrapped("%s", addon.manifest.description.c_str());
    else
        ImGui::TextDisabled("No description.");

    if (!addon.manifest.tags.empty()) {
        ImGui::Dummy(ImVec2(0, S(8)));
        for (const auto& tag : addon.manifest.tags) {
            ImGui::PushStyleColor(ImGuiCol_Button, eam::ui::theme::V(eam::ui::theme::kSelection));
            ImGui::SmallButton(tag.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        ImGui::NewLine();
    }

    if (!addon.manifest.dependencies.empty()) {
        ImGui::Dummy(ImVec2(0, S(8)));
        ImGui::TextDisabled("Dependencies");
        for (const auto& dep : addon.manifest.dependencies) {
            const AddonInfo* found = nullptr;
            for (const auto& other : manager->GetAddons())
                if (other.id == dep) { found = &other; break; }
            ImGui::BulletText("%s", dep.c_str());
            ImGui::SameLine();
            if (!found)                    StatusText("(not installed)", ImVec4(0.816f, 0.502f, 0.502f, 1));
            else if (!found->enabled)      StatusText("(disabled)", eam::ui::theme::V(eam::ui::theme::kWarn));
            else                           ImGui::TextDisabled("(enabled)");
        }
    }

    if (!addon.errorMessage.empty()) {
        ImGui::Dummy(ImVec2(0, S(8)));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.816f, 0.502f, 0.502f, 1));
        ImGui::TextWrapped("Error: %s", addon.errorMessage.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
}

static void RenderConfigPane(const AddonInfo& addon) {
    ImGui::Dummy(ImVec2(0, S(4)));
    if (s_cfgPath != addon.configPath) LoadConfigFile(addon.configPath);

    if (s_cfgTooBig) {
        ImGui::TextWrapped("This file is larger than the editor can hold safely; open it in a text editor instead.");
        return;
    }
    if (eam::ui::Button("Save", eam::ui::icons::kSave, eam::ui::ButtonKind::Primary)) SaveConfigFile();
    widgets::Tip("Write the text below to the addon's config file. Most addons read it only when they load, so a restart may be needed.");
    ImGui::SameLine();
    if (eam::ui::Button("Reload", eam::ui::icons::kReset)) LoadConfigFile(addon.configPath);
    widgets::Tip("Discard your edits and read the file again.");
    if (s_cfgDirty) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, eam::ui::theme::V(eam::ui::theme::kWarn));
        ImGui::TextUnformatted("unsaved changes");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, S(2)));

    ImFont* mono = MonoFont();
    if (mono) ImGui::PushFont(mono, 0.0f);
    if (ImGui::InputTextMultiline("##configedit", s_cfgBuf.data(), s_cfgBuf.size(),
                                  ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_AllowTabInput))
        s_cfgDirty = true;
    if (mono) ImGui::PopFont();
}

static void RenderDetail(AddonManager* manager, AddonInfo& addon, int index) {
    // Header: icon, name (larger), version and author, then the addon's own description; the switch on the right.
    const float iconSize = S(44.0f);
    widgets::DrawAddonIcon(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), iconSize, addon, addon.enabled);
    ImGui::Dummy(ImVec2(iconSize, iconSize));
    ImGui::SameLine(0, S(12));
    ImGui::BeginGroup();
    if (ImFont* title = TitleFont()) ImGui::PushFont(title, ImGui::GetStyle().FontSizeBase * 1.2f);
    ImGui::TextUnformatted(addon.GetDisplayName().c_str());
    if (TitleFont()) ImGui::PopFont();
    ImGui::TextDisabled("v%s  \xc2\xb7  %s", addon.GetDisplayVersion().c_str(), addon.GetDisplayAuthor().c_str());
    ImGui::EndGroup();

    const float switchWidth = ImGui::GetFrameHeight() * 0.8f * 1.8f;
    ImGui::SameLine(ImGui::GetWindowWidth() - switchWidth - ImGui::GetStyle().WindowPadding.x - S(4));
    bool enabled = addon.enabled;
    if (addon.manifest.wip) ImGui::BeginDisabled();
    if (widgets::ToggleSwitch("##detail_enable", &enabled))
        ApplyToggle(manager, index, enabled);
    if (addon.manifest.wip) ImGui::EndDisabled();
    widgets::Tip(addon.manifest.wip ? "Work in progress: this addon cannot be switched on yet."
                                    : "Turn this addon on or off. It loads or unloads at once, unless it says it needs Lossless Scaling restarted.");

    // Status line.
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::TextDisabled("Status");
    ImGui::SameLine();
    if (addon.faulted)            StatusText("Faulted", ImVec4(0.816f, 0.502f, 0.502f, 1));
    else if (addon.IsLoaded())    StatusText("Loaded", eam::ui::theme::V(eam::ui::theme::kAccent));
    else                          ImGui::TextDisabled("Unloaded");

    ImGui::SameLine(0, S(24));
    ImGui::TextDisabled("Security");
    ImGui::SameLine();
    switch (addon.security) {
        case SecurityVerdict::Trusted:  StatusText("Trusted", eam::ui::theme::V(eam::ui::theme::kAccent)); break;
        case SecurityVerdict::Tampered: StatusText("Tampered!", ImVec4(0.816f, 0.502f, 0.502f, 1)); break;
        default:                        ImGui::TextDisabled("Unknown"); break;
    }

    {   // remove, at the right end of the status line
        const float rw = ImGui::CalcTextSize("Remove").x + ImGui::GetFontSize() * 3.2f;
        ImGui::SameLine(ImGui::GetWindowWidth() - rw - ImGui::GetStyle().WindowPadding.x - S(4));
        if (eam::ui::Button("Remove", eam::ui::icons::kTrash, eam::ui::ButtonKind::Danger)) { s_removeId = addon.id; s_askRemove = true; }
        widgets::Tip("Take this addon out of Lossless Scaling. You are asked first. Its folder is moved to addons/.removed, not erased.");
    }

    if (!addon.errorMessage.empty() && addon.IsLoaded()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.816f, 0.502f, 0.502f, 1));
        ImGui::TextWrapped("%s", addon.errorMessage.c_str());
        ImGui::PopStyleColor();
    }
    if (addon.RequiresRestart()) {
        ImGui::PushStyleColor(ImGuiCol_Text, eam::ui::theme::V(eam::ui::theme::kWarn));
        ImGui::TextWrapped("This addon needs Lossless Scaling restarted to apply enable/disable changes.");
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, S(4)));

    // Tabs. The first time an addon is shown, land on its settings if it has any.
    // Capabilities are only known once the DLL is loaded, so an unloaded addon always gets the
    // Settings tab: that is where "Load now" and the reason it is not loaded are shown.
    const bool hasSettings = !addon.IsLoaded() || (addon.capabilities & EAM_CAP_HAS_SETTINGS) != 0;
    const bool hasConfig = !addon.configPath.empty();
    const bool firstShow = (s_tabsOpenedFor != addon.id);
    s_tabsOpenedFor = addon.id;

    if (ImGui::BeginTabBar("##detail_tabs")) {
        if (hasSettings && ImGui::BeginTabItem("Settings", nullptr,
                                               firstShow ? ImGuiTabItemFlags_SetSelected : 0)) {
            RenderSettingsPane(manager, addon, index);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Overview", nullptr,
                                (firstShow && !hasSettings) ? ImGuiTabItemFlags_SetSelected : 0)) {
            RenderOverviewPane(manager, addon);
            ImGui::EndTabItem();
        }
        if (hasConfig && ImGui::BeginTabItem("Config")) {
            RenderConfigPane(addon);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void RenderTabAddons(AddonManager* manager) {
    if (!manager) return;
    auto& addons = manager->GetAddons();

    if (!s_selectionLoaded) {
        s_selectionLoaded = true;
        s_selectedId = ConfigManager::Instance().GlobalGetOr<std::string>("ui", "selected_addon", "");
    }

    auto resolveSelection = [&]() {
        for (int i = 0; i < (int)addons.size(); i++)
            if (addons[i].id == s_selectedId) return i;
        if (!addons.empty()) { SelectAddon(addons[0]); return 0; }   // the remembered one is gone (removed, renamed): pick the first, and remember that
        return -1;
    };
    int sel = resolveSelection();

    // Sidebar (left, slim): the search box, the install buttons and the addons, one row each; the detail pane takes the rest.
    const float avail = ImGui::GetContentRegionAvail().x;
    const float listWidth = std::clamp(avail * 0.26f, S(220.0f), S(280.0f));
    ImGui::BeginChild("AddonList", ImVec2(listWidth, -1), false);

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F)) ImGui::SetKeyboardFocusHere();   // Ctrl+F: jump to the search box
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##addon_search", "Search (Ctrl+F)", s_searchBuffer, sizeof(s_searchBuffer));
    widgets::Tip("Filter the list by name, author or tag.");
    ImGui::Dummy(ImVec2(0, S(2)));

    if (eam::ui::Button("Install addon", eam::ui::icons::kDownload, eam::ui::ButtonKind::Primary)) ImGui::OpenPopup("##install_menu");
    widgets::Tip("Add an addon from a folder, a .zip or a .dll. It is copied into the addons folder and installed switched off. "
                 "Dropping one onto this window works too.");
    if (ImGui::BeginPopup("##install_menu")) {
        std::wstring picked;
        if (ImGui::Selectable("From a folder...") && PickPath(true, picked)) { s_installSource = picked; s_askInstall = true; }
        if (ImGui::Selectable("From a zip or DLL...") && PickPath(false, picked)) { s_installSource = picked; s_askInstall = true; }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (eam::ui::IconButton("##open_folder", eam::ui::icons::kFolder, ImGui::GetFrameHeight())) ShellExecuteW(nullptr, L"open", manager->GetAddonsPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    widgets::Tip("Open the folder the addons live in, to remove or update one by hand.");

    if (s_askInstall) { ImGui::OpenPopup("Install addon"); s_askInstall = false; }
    if (ImGui::BeginPopupModal("Install addon", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextWrapped("Install this addon?");
        ImGui::TextDisabled("%s", WideToUtf8(s_installSource).c_str());
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::TextWrapped("An addon is a program that runs inside Lossless Scaling with the same access as Lossless Scaling itself. "
                           "Only install addons from people you trust. It is installed switched off; you turn it on with its switch.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, S(6)));
        if (eam::ui::Button("Install", eam::ui::icons::kDownload, eam::ui::ButtonKind::Primary)) {
            const AddonManager::InstallResult r = manager->InstallAddon(s_installSource);
            widgets::ToastShow(r.message, r.ok ? widgets::ToastType::Success : widgets::ToastType::Error, 7.0f);
            if (r.ok) s_selectedId = r.id;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (eam::ui::Button("Cancel", eam::ui::icons::kClose)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (s_askRemove) { ImGui::OpenPopup("Remove addon"); s_askRemove = false; }
    if (ImGui::BeginPopupModal("Remove addon", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        int ri = -1;
        for (int i = 0; i < (int)addons.size(); ++i) if (addons[i].id == s_removeId) ri = i;
        if (ri < 0) { ImGui::TextUnformatted("That addon is already gone."); if (eam::ui::Button("Close")) ImGui::CloseCurrentPopup(); }
        else {
            const std::string nm = addons[ri].GetDisplayName();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
            ImGui::TextWrapped("Remove %s?", nm.c_str());
            ImGui::Dummy(ImVec2(0, S(4)));
            ImGui::TextWrapped("It is switched off and its folder is moved to addons/.removed (nothing is erased; move the folder back to restore it). "
                               "Its settings are kept, so installing it again brings them back.");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, S(6)));
            if (eam::ui::Button("Remove", eam::ui::icons::kTrash, eam::ui::ButtonKind::Danger)) {
                const AddonManager::InstallResult rr = manager->RemoveAddon(ri);
                widgets::ToastShow(rr.message, rr.ok ? widgets::ToastType::Success : widgets::ToastType::Error, 7.0f);
                if (rr.ok) s_selectedId.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (eam::ui::Button("Cancel", eam::ui::icons::kClose)) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::PushStyleColor(ImGuiCol_Separator, eam::ui::theme::V(eam::ui::theme::kBorder));
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(4)));
    if (addons.empty()) {
        widgets::EmptyState(listWidth, "No addons installed yet", "Use Install addon above, or drop a folder, .zip or .dll onto this window.");
    }
    for (int i = 0; i < (int)addons.size(); i++) {
        if (!MatchesFilter(addons[i], s_searchBuffer)) continue;
        bool toggled = false;
        if (widgets::AddonCard(addons[i], i, i == sel, &toggled))
            SelectAddon(addons[i]);
        if (toggled) ApplyToggle(manager, i, addons[i].enabled);
        ImGui::Dummy(ImVec2(0, S(1)));
    }
    // the runtime files the addons use, at the bottom of the list (right under the addons when they fill it)
    {
        const std::wstring lsDir = std::filesystem::path(manager->GetAddonsPath()).parent_path().wstring();
        const std::vector<RuntimeFile> runtimes = RuntimeFiles(addons, lsDir);
        if (!runtimes.empty()) widgets::RuntimeListAtBottom(runtimes);
    }
    ImGui::EndChild();

    sel = resolveSelection();
    ImGui::SameLine();
    ImGui::BeginChild("AddonDetail", ImVec2(0, -1), true);
    if (sel >= 0)
        RenderDetail(manager, addons[sel], sel);
    else
        ImGui::TextDisabled("Select an addon.");
    ImGui::EndChild();
}

} // namespace eam
