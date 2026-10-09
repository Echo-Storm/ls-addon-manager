#include "addon_manager.h"
#include "addon_dependency.h"
#include "addon_discovery.h"
#include "addon_install.h"
#include "addon_manifest.h"
#include "addon_runtime.h"
#include "addon_security.h"
#include "../config/config_manager.h"
#include "../event/event_system.h"
#include "../features/features.h"
#include "../gui/icon_loader.h"
#include "../host/host_impl.h"
#include "../log/logger.h"
#include "../../sdk/include/eam/version.h"
#include "imgui.h"
#include <algorithm>
#include <system_error>
#include <unordered_set>
#include <windows.h>

namespace fs = std::filesystem;

namespace eam {

namespace {

// Settings > Security: 0 allow everything, 1 warn about anything not on trusted_addons.json, 2 refuse it.
int SecurityLevel() { return ConfigManager::Instance().GlobalGetOr<int>(nullptr, "security_level", 0); }

void Announce(uint32_t event, const AddonInfo& addon) {
    EamAddonEventData data;
    data.addonName = addon.GetDisplayName().c_str();
    data.addonVersion = addon.GetDisplayVersion().c_str();
    EventBus::Instance().Publish(event, &data, sizeof data);
}

std::wstring DefaultAddonsPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return (fs::path(exe).parent_path() / "addons").wstring();
}

bool InRange(const std::vector<AddonInfo>& list, int index) { return index >= 0 && index < (int)list.size(); }

} // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------------------------------------------------------------

AddonManager::AddonManager(HostImpl* host) : AddonManager(host, DefaultAddonsPath()) {}

AddonManager::AddonManager(HostImpl* host, const std::wstring& addonsPath)
    : m_host(host), m_addonsPath(addonsPath), m_configPath((fs::path(addonsPath) / "config.json").wstring()) {
    std::error_code ec;
    fs::create_directories(m_addonsPath, ec);
    AddonSecurity::LoadTrustedHashes(m_addonsPath);
    ConfigManager::Instance().Load(m_configPath);
}

AddonManager::~AddonManager() {
    UnloadAddons();
    for (AddonInfo& addon : m_addons) ReleaseIcon(addon);
}

void AddonManager::ReleaseIcon(AddonInfo& addon) {
    if (addon.iconTexture) { addon.iconTexture->Release(); addon.iconTexture = nullptr; }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Finding addons
// ---------------------------------------------------------------------------------------------------------------------------------

bool AddonManager::Inspect(const fs::path& folder, AddonInfo& info) {
    if (!DiscoverAddon(folder, info)) return false;
    for (const std::string& old : info.manifest.renamedFrom)   // it used to be called something else: keep its settings
        if (ConfigManager::Instance().RenameAddonSection(old, info.id)) {
            LOG_INFO("AddonManager", "Carried the settings of '%s' over to '%s'", old.c_str(), info.id.c_str());
            m_settingsMoved = true;
        }
    // a work in progress stays off; one never switched on or off yet follows its "enabled_by_default"
    info.enabled = !info.manifest.wip && ConfigManager::Instance().IsAddonEnabled(info.id, info.manifest.enabledByDefault);
    info.security = AddonSecurity::VerifyDll(info.dllPath, info.id);
    return true;
}

void AddonManager::ScanAddons() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (AddonInfo& addon : m_addons) ReleaseIcon(addon);
    m_addons.clear();

    std::error_code ec;
    for (fs::directory_iterator it(m_addonsPath, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& folder = it->path();
        if (!it->is_directory(ec)) continue;
        if (folder.filename().wstring().front() == L'.') continue;   // .removed and the .install-* staging folders
        if (features::IsRetiredAddonId(WideToUtf8(folder.filename().wstring()))) {   // now part of the manager; running both would hook twice
            LOG_INFO("AddonManager", "Ignoring the folder '%s': that addon is built in now", WideToUtf8(folder.filename().wstring()).c_str());
            continue;
        }
        AddonInfo info;
        if (Inspect(folder, info)) m_addons.push_back(std::move(info));
    }

    // An addon that was renamed hides the folder it used to have (running both would be the same addon twice).
    std::unordered_set<std::string> superseded;
    for (const AddonInfo& a : m_addons)
        for (const std::string& old : a.manifest.renamedFrom) superseded.insert(old);
    m_addons.erase(std::remove_if(m_addons.begin(), m_addons.end(), [&](const AddonInfo& a) {
        if (!superseded.count(a.id)) return false;
        LOG_INFO("AddonManager", "Ignoring the folder '%s': the addon was renamed and its new folder is installed", a.id.c_str());
        return true;
    }), m_addons.end());
    if (m_settingsMoved) { ConfigManager::Instance().Save(); m_settingsMoved = false; }

    AddonDependency::Resolve(m_addons);
    LOG_INFO("AddonManager", "Scanned %zu addons", m_addons.size());
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Installing and removing (the file work is in addon_install.cpp)
// ---------------------------------------------------------------------------------------------------------------------------------

AddonManager::InstallResult AddonManager::InstallAddon(const std::wstring& source) {
    InstallResult result;
    const PlaceResult placed = PlaceAddon(fs::path(source), fs::path(m_addonsPath));
    if (!placed.ok) { result.message = placed.message; return result; }

    AddonInfo info;
    if (!Inspect(placed.dest, info)) {
        std::error_code ec;
        fs::remove_all(placed.dest, ec);   // the folder PlaceAddon just made, which holds nothing usable
        result.message = "That addon has no DLL, so there is nothing to load.";
        return result;
    }

    // An addon is code that runs inside Lossless Scaling, so it arrives switched off and the user turns it on on purpose.
    info.enabled = false;
    ConfigManager::Instance().SetAddonEnabled(info.id, false);
    ConfigManager::Instance().Save();

    result.ok = true;
    result.id = info.id;
    result.message = "Installed '" + info.GetDisplayName() + "' (switched off). Turn it on with its switch.";
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_addons.push_back(std::move(info));
        AddonDependency::Resolve(m_addons);
    }
    LoadAddonIcons();
    LOG_INFO("AddonManager", "Installed addon '%s' from %s", result.id.c_str(), WideToUtf8(source).c_str());
    return result;
}

AddonManager::InstallResult AddonManager::RemoveAddon(int index) {
    InstallResult result;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!InRange(m_addons, index)) { result.message = "That addon is no longer in the list."; return result; }

    AddonInfo& addon = m_addons[index];
    const std::string name = addon.GetDisplayName();
    const std::string id = addon.id;

    if (addon.IsLoaded()) {
        if (addon.RequiresRestart()) {
            result.message = "'" + name + "' is loaded and needs Lossless Scaling restarted before it can be removed. "
                             "Switch it off, restart Lossless Scaling, then remove it.";
            return result;
        }
        UnloadModule(addon);
        if (addon.IsLoaded()) { result.message = "Could not unload '" + name + "'; restart Lossless Scaling and try again."; return result; }
    }

    const RemoveResult moved = MoveAddonToRemoved(fs::path(m_addonsPath) / addon.folderName, fs::path(m_addonsPath));
    if (!moved.ok) { result.message = moved.message; return result; }

    ReleaseIcon(addon);
    m_addons.erase(m_addons.begin() + index);
    AddonDependency::Resolve(m_addons);
    // If it is installed again it starts switched off; its other settings are kept.
    ConfigManager::Instance().SetAddonEnabled(id, false);
    ConfigManager::Instance().Save();

    result.ok = true;
    result.id = id;
    result.message = "Removed '" + name + "'. Its folder is in addons/.removed (move it back to restore it); its settings are kept.";
    LOG_INFO("AddonManager", "Removed addon '%s' -> %s", id.c_str(), WideToUtf8(moved.movedTo.wstring()).c_str());
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Loading and starting
// ---------------------------------------------------------------------------------------------------------------------------------

std::string AddonManager::WhyNotLoadable(const AddonInfo& addon) {
    if (!addon.manifest.minHostVersion.empty() && ParseApiVersion(addon.manifest.minHostVersion) > (uint32_t)EAM_API_VERSION_INT) {
        const std::string why = "Needs a newer " EAM_PRODUCT_NAME " (addon API " + addon.manifest.minHostVersion +
                                " or newer; this one provides " EAM_API_VERSION_STRING ")";
        LOG_ERROR("AddonManager", "Not loading '%s': %s", addon.id.c_str(), why.c_str());
        return why;
    }

    const int level = SecurityLevel();
    if (level >= 1 && addon.security != SecurityVerdict::Trusted) {
        const char* problem = addon.security == SecurityVerdict::Tampered ? "does not match its trusted hash" : "is not on the trusted list";
        if (level >= 2) {
            const std::string why = std::string("Blocked: the DLL ") + problem + " (Settings > Security Level)";
            LOG_WARN("AddonManager", "Not loading '%s': %s", addon.id.c_str(), why.c_str());
            return why;
        }
        LOG_WARN("AddonManager", "Loading '%s' although its DLL %s", addon.id.c_str(), problem);
    }
    return {};
}

void AddonManager::FillFromExports(AddonInfo& addon) {
    struct Field { std::string* into; const char* (*getter)(); };
    const Field fields[] = {
        { &addon.manifest.name, addon.exports.name },
        { &addon.manifest.version, addon.exports.version },
        { &addon.manifest.author, addon.exports.author },
        { &addon.manifest.description, addon.exports.description },
    };
    for (const Field& f : fields) {
        if (!f.into->empty()) continue;   // addon.json wins
        if (const char* text = guarded::Text(f.getter)) *f.into = text;
    }
}

void AddonManager::LoadModule(AddonInfo& addon) {
    addon.errorMessage = WhyNotLoadable(addon);
    if (!addon.errorMessage.empty()) return;

    HMODULE module = LoadLibraryExW(addon.dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) module = LoadLibraryW(addon.dllPath.c_str());
    if (!module) {
        addon.errorMessage = "LoadLibrary failed (error " + std::to_string(GetLastError()) + ")";
        LOG_ERROR("AddonManager", "Failed to load '%s': %s", addon.id.c_str(), addon.errorMessage.c_str());
        return;
    }

    addon.hModule = module;
    addon.faulted = false;
    BindExports(module, addon.exports);
    addon.capabilities = guarded::Capabilities(module);
    FillFromExports(addon);
    LOG_INFO("AddonManager", "Loaded addon '%s' v%s", addon.GetDisplayName().c_str(), addon.GetDisplayVersion().c_str());
}

bool AddonManager::StartAddon(AddonInfo& addon, ImGuiContext* ctx) {
    if (!addon.IsLoaded() || !addon.exports.init || !ctx) return false;

    ImGuiMemAllocFunc alloc = nullptr;
    ImGuiMemFreeFunc release = nullptr;
    void* userData = nullptr;
    ImGui::GetAllocatorFunctions(&alloc, &release, &userData);

    if (!guarded::Initialize(addon.exports, m_host, ctx, (void*)alloc, (void*)release, userData)) {
        addon.faulted = true;
        addon.errorMessage = "Crashed during initialization";
        LOG_ERROR("AddonManager", "Addon '%s' crashed during init!", addon.id.c_str());
        return false;
    }
    Announce(EAM_EVENT_ADDON_LOADED, addon);
    return true;
}

void AddonManager::UnloadModule(AddonInfo& addon) {
    if (!addon.IsLoaded()) return;
    guarded::Shutdown(addon.exports);
    Announce(EAM_EVENT_ADDON_UNLOADED, addon);
    // Whatever the addon left registered would point into freed code once its DLL is gone (a dispatch callback on Lossless Scaling's render
    // thread): take back every event subscription and dispatch callback whose code lies in the DLL's image.
    const auto base = reinterpret_cast<uintptr_t>(addon.hModule);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(addon.hModule);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    const size_t left = EventBus::Instance().ForgetCode(base, end) + (m_host ? m_host->ForgetCode(base, end) : 0);
    if (left) LOG_WARN("AddonManager", "'%s' left %zu callback(s) registered when it shut down; removed them", addon.id.c_str(), left);
    FreeLibrary(addon.hModule);
    addon.hModule = nullptr;
    addon.exports = AddonExports{};
    addon.capabilities = 0;
    addon.faulted = false;
}

namespace {
// Two addons that cannot run side by side (either one names the other under "conflicts").
bool Conflict(const AddonInfo& a, const AddonInfo& b) {
    auto names = [](const AddonInfo& x, const AddonInfo& y) {
        return std::find(x.manifest.conflicts.begin(), x.manifest.conflicts.end(), y.id) != x.manifest.conflicts.end();
    };
    return &a != &b && (names(a, b) || names(b, a));
}
} // namespace

void AddonManager::SwitchOff(AddonInfo& addon) {
    addon.enabled = false;
    if (!addon.RequiresRestart()) UnloadModule(addon);
    ConfigManager::Instance().SetAddonEnabled(addon.id, false);
}

void AddonManager::LoadAddons() {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Two addons that conflict and are both switched on (from an older version, or config.json edited by hand): the first in the list stays on.
    bool changed = false;
    for (size_t i = 0; i < m_addons.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (m_addons[i].enabled && m_addons[j].enabled && Conflict(m_addons[i], m_addons[j])) {
                LOG_WARN("AddonManager", "'%s' and '%s' cannot run side by side: '%s' is switched off", m_addons[j].id.c_str(), m_addons[i].id.c_str(), m_addons[i].id.c_str());
                SwitchOff(m_addons[i]);
                changed = true;
            }
    if (changed) ConfigManager::Instance().Save();
    for (AddonInfo& addon : m_addons)
        if (addon.enabled && !addon.IsLoaded()) LoadModule(addon);
}

void AddonManager::InitializeAddons(ImGuiContext* ctx) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (AddonInfo& addon : m_addons)
        if (addon.enabled) StartAddon(addon, ctx);
}

void AddonManager::UnloadAddons() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_addons.rbegin(); it != m_addons.rend(); ++it) UnloadModule(*it);   // dependents first
}

// ---------------------------------------------------------------------------------------------------------------------------------
// While it runs
// ---------------------------------------------------------------------------------------------------------------------------------

std::vector<std::string> AddonManager::ToggleAddon(int index, bool enable) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> switchedOff;
    if (!InRange(m_addons, index)) return switchedOff;
    AddonInfo& addon = m_addons[index];
    if (enable && addon.manifest.wip) {   // work in progress: it cannot be switched on (the switch is greyed out; this is the guard)
        addon.enabled = false;
        return switchedOff;
    }
    if (enable)   // first the addons it cannot run beside, so they have let go of whatever they held
        for (AddonInfo& other : m_addons)
            if (other.enabled && Conflict(addon, other)) {
                LOG_INFO("AddonManager", "Turning '%s' off: it cannot run beside '%s'", other.id.c_str(), addon.id.c_str());
                SwitchOff(other);
                switchedOff.push_back(other.GetDisplayName());
            }
    addon.enabled = enable;

    if (addon.RequiresRestart()) {
        // It cannot be loaded or unloaded under a running Lossless Scaling: only remember the choice for the next start.
        LOG_INFO("AddonManager", "Addon '%s' requires restart to %s", addon.GetDisplayName().c_str(), enable ? "enable" : "disable");
    } else if (enable && !addon.IsLoaded()) {
        LoadModule(addon);
        StartAddon(addon, ImGui::GetCurrentContext());
    } else if (!enable) {
        UnloadModule(addon);
    }

    ConfigManager::Instance().SetAddonEnabled(addon.id, enable);
    ConfigManager::Instance().Save();
    return switchedOff;
}

void AddonManager::LoadAddonNow(int index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!InRange(m_addons, index)) return;
    AddonInfo& addon = m_addons[index];
    if (!addon.enabled || addon.IsLoaded()) return;
    LoadModule(addon);
    StartAddon(addon, ImGui::GetCurrentContext());
}

void AddonManager::RenderAddonSettings(int index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!InRange(m_addons, index)) return;
    AddonInfo& addon = m_addons[index];
    if (!addon.IsLoaded() || !addon.exports.renderSettings || addon.settingsFaulted) return;

    // The panel is drawn every frame: one that faults, or passes a bad argument to a CRT function (after which what it does is undefined), would
    // do it again sixty times a second, each time leaving the window's drawing half done. It is switched off for the session after the first.
    const uint32_t invalidBefore = InvalidParameterCount();
    const bool drawn = guarded::RenderSettings(addon.exports);
    if (!drawn || InvalidParameterCount() != invalidBefore) {
        addon.settingsFaulted = true;
        addon.errorMessage = drawn ? "Its settings panel hit an invalid parameter and was turned off (see Logs)" : "Its settings panel crashed and was turned off (see Logs)";
        LOG_ERROR("AddonManager", "Settings panel of '%s' %s; not drawing it again this session", addon.id.c_str(), drawn ? "hit an invalid CRT parameter" : "faulted");
    }
}

bool AddonManager::InterceptResource(const wchar_t* name, const wchar_t* type, const void** outData, uint32_t* outSize) {
    // Lossless Scaling's own thread asks (FindResourceW). The list lock is also held while an addon draws its settings panel or starts, which can take long: this waits for it
    // at most 50 ms and then lets Lossless Scaling have its own resource, rather than stall it for as long as the panel does.
    std::unique_lock<std::mutex> lock(m_mutex, std::defer_lock);
    for (int tries = 0; !lock.try_lock(); ++tries) {
        if (tries >= 50) {
            static bool said = false;
            if (!said) { said = true; LOG_WARN("AddonManager", "A resource lookup of Lossless Scaling did not wait for the addon list (busy for over 50 ms); it got its own resource"); }
            return false;
        }
        Sleep(1);
    }
    for (const AddonInfo& addon : m_addons) {
        if (!addon.enabled || addon.faulted) continue;
        if (guarded::Intercept(addon.exports, name, type, outData, outSize)) return true;
    }
    return false;
}

void AddonManager::LoadAddonIcons() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (AddonInfo& addon : m_addons)
        if (!addon.iconTexture && !addon.iconPath.empty()) addon.iconTexture = LoadIconTexture(addon.iconPath);
}

std::vector<AddonInfo>& AddonManager::GetAddons() { return m_addons; }
std::mutex& AddonManager::GetMutex() { return m_mutex; }

} // namespace eam
