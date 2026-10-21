#pragma once
#include "../../sdk/include/eam/addon_exports.h"
#include <cstdint>
#include <d3d11.h>
#include <string>
#include <vector>
#include <windows.h>

namespace eam {

// What trusted_addons.json says about an addon's DLL.
enum class SecurityVerdict : uint8_t {
    Unknown,    // there is nothing to compare with
    Trusted,    // the DLL's SHA-256 is on the list
    Tampered,   // the addon is on the list but this DLL is not the listed one
    Unsigned    // not used yet
};

// addon.json, or what the DLL itself reports when a field is missing.
struct AddonManifest {
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string minHostVersion;              // the lowest addon API it can run on, "1.0.0"
    std::string dll;                         // a DLL name other than the folder's
    std::string icon;                        // an icon file name other than icon.png
    std::vector<std::string> dependencies;   // ids of addons that must load first
    std::vector<std::string> renamedFrom;    // folder names this addon used to have: its settings are carried over, and those folders are hidden
    std::vector<std::string> conflicts;      // ids of addons that cannot run beside this one: turning either on turns the other off
    bool wip = false;                        // work in progress: listed, but it cannot be switched on and is never loaded
    bool enabledByDefault = true;            // false: a newly installed addon stays off until the person switches it on
    std::vector<std::string> tags;
    // "runtimes": files the addon runs on that are not its own (NVIDIA's DLSS, AMD's FidelityFX, a model file), listed at the bottom of the
    // addon list with their version and signature (runtime_files.h)
    struct Runtime {
        std::string name;            // "FSR"
        std::string file;            // relative to the addon's folder, or "{ls}/..." for Lossless Scaling's folder
        std::string configKey;       // an addon setting that holds the file's path when the person chose one ("snippetPath")
        std::string shippedSha256;   // the file the addon ships with, and the version to show for it
        std::string shippedLabel;
        std::vector<std::string> exports;   // functions a file must offer to be taken for this runtime (a check when one is chosen)
    };
    std::vector<Runtime> runtimes;
    bool parsed = false;                     // addon.json was found and was valid
};

// The functions a loaded addon's DLL offers. Every one of them may be missing.
struct AddonExports {
    AddonInit_t init = nullptr;
    AddonShutdown_t shutdown = nullptr;
    AddonRenderSettings_t renderSettings = nullptr;
    AddonInterceptResource_t intercept = nullptr;
    GetAddonName_t name = nullptr;
    GetAddonVersion_t version = nullptr;
    GetAddonAuthor_t author = nullptr;
    GetAddonDescription_t description = nullptr;
};

// Is version a older than version b (numbers up to three parts: "1.0.3")? A text that is not numbers counts as equal.
inline bool VersionOlder(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& t, int out[3]) { out[0] = out[1] = out[2] = 0; int i = 0; size_t at = 0; bool any = false;
        while (i < 3 && at < t.size() && t[at] >= '0' && t[at] <= '9') { int v = 0; while (at < t.size() && t[at] >= '0' && t[at] <= '9') { v = v * 10 + (t[at] - '0'); ++at; } out[i++] = v; any = true; if (at < t.size() && t[at] == '.') ++at; else break; }
        return any; };
    int x[3], y[3];
    if (!parts(a, x) || !parts(b, y)) return false;
    for (int i = 0; i < 3; ++i) if (x[i] != y[i]) return x[i] < y[i];
    return false;
}

struct AddonInfo {
    // who it is
    std::wstring folderName;   // the folder under addons\, which is also the addon's id
    std::string id;            // the same, in UTF-8
    std::wstring dllPath;
    std::wstring configPath;   // an .ini file the addon ships with, if any
    AddonManifest manifest;

    // what state it is in
    bool enabled = true;                   // the user's switch
    HMODULE hModule = nullptr;             // non-null while the DLL is loaded
    uint32_t capabilities = 0;             // EAM_CAP_* bits, read from the DLL
    SecurityVerdict security = SecurityVerdict::Unknown;
    std::string errorMessage;              // why it is not running, in words for the user
    bool faulted = false;                  // it crashed while starting
    bool settingsFaulted = false;          // its settings panel hit an invalid CRT parameter and is no longer drawn
    AddonExports exports;
    std::string dllVersion;                // what the loaded DLL itself says its version is (GetAddonVersion): the manifest's can be newer than the file (a test build left in the folder)

    // its icon
    std::wstring iconPath;
    ID3D11ShaderResourceView* iconTexture = nullptr;   // released by the manager
    // or a vector icon (icon.svg): the path data of its shapes, drawn in the theme's colours at any size; iconSvgView is its viewBox width
    std::vector<std::string> iconSvg;
    float iconSvgView = 24.0f;

    // the window's per-addon state
    bool showSettings = false;
    bool selected = false;

    const std::string& GetDisplayName() const { return manifest.name.empty() ? id : manifest.name; }
    const std::string& GetDisplayVersion() const {
        static const std::string kUnknown = "?.?.?";
        return manifest.version.empty() ? kUnknown : manifest.version;
    }
    const std::string& GetDisplayAuthor() const {
        static const std::string kUnknown = "Unknown";
        return manifest.author.empty() ? kUnknown : manifest.author;
    }
    bool IsLoaded() const { return hModule != nullptr; }
    // One of ours (Echo-Storm's) whose DLL says an older version than the manager's: a test build or an old file left in the folder, which Setup replaces (Reinstall)
    bool OlderBuild(const std::string& managerVersion) const { return IsLoaded() && manifest.author == "Echo-Storm" && !dllVersion.empty() && VersionOlder(dllVersion, managerVersion); }
    bool RequiresRestart() const { return (capabilities & EAM_CAP_REQUIRES_RESTART) != 0; }
};

} // namespace eam
