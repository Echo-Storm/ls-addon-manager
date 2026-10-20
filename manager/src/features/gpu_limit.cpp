#include "gpu_limit.h"
#include "../config/config_manager.h"
#include "../log/logger.h"
#include "imgui.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace eam {
namespace features {
namespace gpulimit {

namespace {

ConfigManager& Cfg() { return ConfigManager::Instance(); }
Backends g_backends;
std::atomic<bool> g_on{ false };

uint32_t Percent() { return static_cast<uint32_t>(std::clamp(std::atoi(Cfg().Get(kId, "percent", "95").c_str()), 80, 100)); }
uint32_t Load() { return g_backends.readLoad ? g_backends.readLoad() : 0u; }

} // namespace

void SetBackends(const Backends& backends) { g_backends = backends; }

void Start() {
    g_on = true;
    if (g_backends.setLimit) g_backends.setLimit(Percent());
    LOG_INFO("GpuLimit", "on: keep the card under %u %%", Percent());
}

void Stop() {
    if (!g_on.exchange(false)) return;
    if (g_backends.setLimit) g_backends.setLimit(0);
    LOG_INFO("GpuLimit", "off");
}

std::string Status() {
    if (!g_on) return {};
    const uint32_t limit = Percent(), load = Load();
    char text[120];
    if (!load) snprintf(text, sizeof text, "Limit %u %%", limit);
    else snprintf(text, sizeof text, "Limit %u %%: the card is at %u %%%s", limit, load, load >= limit ? " (over: addons are cutting their cost)" : "");
    return text;
}

void RenderOptions() {
    int percent = static_cast<int>(Percent());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::SliderInt("Keep the card under", &percent, 80, 100, "%d %%")) {
        Cfg().Set(kId, "percent", std::to_string(percent));
        Cfg().Save();
        if (g_backends.setLimit && g_on) g_backends.setLimit(static_cast<uint32_t>(percent));
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("When the graphics card's load stays at or above this for a few seconds, the addons make their own work cheaper (Neural Rendering takes the model's working size down; the upscalers' \"lighter upscaling when the card is full\" starts), and "
                          "go back when the load is well under it. 100 % means only when the card is completely full. It works together with the addons' own settings: Neural Rendering's Auto quality has to be on for it.");
    const uint32_t load = Load();
    if (load) ImGui::TextDisabled("The card is at %u %% now%s.", load, load >= static_cast<uint32_t>(percent) ? ": over the limit" : "");
    else ImGui::TextDisabled("The card's load is read while a game is scaled (or reads as idle).");
    ImGui::TextDisabled("It reads the card Lossless Scaling runs on; no addon has to be switched on, but it only has an effect on the addons that are.");
}

} // namespace gpulimit
} // namespace features
} // namespace eam
