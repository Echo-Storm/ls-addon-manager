#include "features.h"
#include "gpu_limit.h"
#include "ls_config.h"
#include "recorder_feature.h"
#include "reshade_passthrough.h"
#include "windowed_mode.h"
#include "../config/config_manager.h"
#include "../log/logger.h"
#include <cstring>

namespace eam {
namespace features {

namespace {

enum Which { kReShade = 0, kWindowed = 1, kRecorder = 2, kGpuLimit = 3, kLsConfig = 4, kCount = 5 };

const Info kInfo[kCount] = {
    { reshade::kId,
      "ReShade input passthrough",
      "Use the mouse and keyboard on a ReShade overlay while Lossless Scaling is scaling the game.",
      "Lossless Scaling's window normally swallows the mouse and keyboard, so a ReShade menu cannot be clicked or typed into. With this on, "
      "the hotkey below turns passthrough on and off: while it is on, your input goes to the ReShade overlay. It turns itself off when the "
      "game or Lossless Scaling loses focus.\nIt can be switched on and off at any time." },
    { windowed::kId,
      "Windowed mode and second monitor",
      "Let Lossless Scaling work with a windowed game, or with a second monitor.",
      "Adds a virtual display the size of your game window, so Lossless Scaling can scale a window instead of needing full screen. It can also "
      "cover just one half of that window (split screen) or sit beside it (for a second monitor).\nIt has to be in place before Lossless "
      "Scaling starts, so switching it on needs Lossless Scaling to be restarted. Switching it off takes effect at once." },
    { recorder::kId,
      "Recording",
      "Keep the last seconds of what Lossless Scaling captures and save them with a hotkey.",
      "The last few seconds of the game's frames (as Lossless Scaling captures them), kept in memory, losslessly compressed, and saved as a .lsrec file with "
      "Ctrl+Shift+F1: for bug reports that can be replayed offline, tests and comparing looks. It finds the frames by itself, with frame generation on and "
      "no addon switched on (with frame generation off it takes the frame Lossless Scaling's scaling pass reads); the addons add the frames only they have. It can be switched on and off at any time." },
    { gpulimit::kId,
      "GPU limit",
      "Keep the graphics card under a load, 80 to 100 %, for the addons that can make their work cheaper.",
      "The manager reads the load of the card Lossless Scaling runs on. While it stays at or over the limit for a few seconds, the addons lower their cost: Neural Rendering's Auto quality "
      "takes the model's working size down, and the upscalers' \"lighter upscaling when the card is full\" starts; they go back when the load is well under the limit. One setting for all the "
      "addons. It can be switched on and off at any time." },
    { lsconfig::kId,
      "Lossless Scaling's hidden settings",
      "Change the look and place of its frame rate counter, and a few expert values, that its window does not show.",
      "Edits Lossless Scaling's own config.ini (beside this program): the counter's size, colour, font and position, and some expert values. Lossless Scaling reads the file when scaling starts, so a change applies from the next time. Only the lines you change are touched; the first change keeps a copy as config.ini.bak-manager." },
};

ConfigManager& Cfg() { return ConfigManager::Instance(); }

} // namespace

int Count() { return kCount; }

const Info& At(int index) { return kInfo[index]; }

bool IsOn(int index) { return Cfg().IsAddonEnabled(kInfo[index].id, false); }

void SetOn(int index, bool on) {
    Cfg().SetAddonEnabled(kInfo[index].id, on);
    Cfg().Save();
    LOG_INFO("Features", "%s is now %s", kInfo[index].title, on ? "on" : "off");

    switch (index) {
        case kReShade:
            if (on) reshade::Start(); else reshade::Stop();
            break;
        case kWindowed:
            if (windowed::Started()) windowed::SetActive(on);   // hooks are in: the switch is live. Otherwise it waits for a restart.
            break;
        case kRecorder:
            if (on) recorder::Start(); else recorder::Stop();
            break;
        case kGpuLimit:
            if (on) gpulimit::Start(); else gpulimit::Stop();
            break;
    }
}

bool NeedsRestart(int index) { return index == kWindowed && IsOn(index) && !windowed::Started(); }

std::string Status(int index) {
    switch (index) {
        case kReShade: return IsOn(index) ? reshade::Status() : std::string();
        case kWindowed: return IsOn(index) ? windowed::Status() : std::string();
        case kRecorder: return IsOn(index) ? recorder::Status() : std::string();
        case kGpuLimit: return IsOn(index) ? gpulimit::Status() : std::string();
        case kLsConfig: return {};
    }
    return {};
}

void RenderOptions(int index) {
    switch (index) {
        case kReShade: reshade::RenderOptions(); break;
        case kWindowed: windowed::RenderOptions(); break;
        case kRecorder: recorder::RenderOptions(); break;
        case kGpuLimit: gpulimit::RenderOptions(); break;
        case kLsConfig: lsconfig::RenderOptions(); break;
    }
}

// Up to 0.7.4 the features kept their settings under the ids of the standalone addons they came from; move them once.
void MoveOldSettings() {
    const char* const oldIds[kCount] = { reshade::kOldId, windowed::kOldId, recorder::kId, gpulimit::kId, lsconfig::kId };   // (the recorder is new: its old id is its own, which renames nothing)
    bool moved = false;
    for (int i = 0; i < kCount; ++i)
        if (Cfg().RenameAddonSection(oldIds[i], kInfo[i].id)) { moved = true; LOG_INFO("Features", "%s: settings moved from %s to %s", kInfo[i].title, oldIds[i], kInfo[i].id); }
    if (moved) Cfg().Save();
}

void Start() {
    MoveOldSettings();
    if (IsOn(kReShade)) reshade::Start();
    if (IsOn(kWindowed)) windowed::Start();
    if (IsOn(kRecorder)) recorder::Start();
    if (IsOn(kGpuLimit)) gpulimit::Start();
}

void Stop() {
    gpulimit::Stop();
    recorder::Stop();
    reshade::Stop();
    windowed::Stop();
}

bool IsRetiredAddonId(const std::string& folderName) {
    return folderName == reshade::kOldId || folderName == windowed::kOldId;
}

} // namespace features
} // namespace eam
