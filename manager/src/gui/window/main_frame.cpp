#include "../widgets/update_offer.h"
#include "main_frame.h"
#include "status_text.h"
#include "../tabs/tab_about.h"
#include "../tabs/tab_addons.h"
#include "../tabs/tab_features.h"
#include "../tabs/tab_logs.h"
#include "../tabs/tab_performance.h"
#include "../tabs/tab_settings.h"
#include "../widgets/status_bar.h"
#include "../widgets/header_bar.h"
#include "../../addon/addon_manager.h"
#include "../../host/metrics.h"
#include "../../update/update_check.h"
#include "imgui.h"

namespace eam {
namespace window {

namespace {

std::string BuildStatusLine(AddonManager* manager) {
    int total = 0, on = 0;
    if (manager) for (const auto& addon : manager->GetAddons()) { ++total; if (addon.enabled) ++on; }
    std::string line = StatusCounts(total, on);
    const update::Status newer = update::Current();
    line = WithUpdate(line, newer.state == update::State::Available ? newer.latest : std::string());

    const Metrics::Status live = Metrics::Instance().BestStatus();   // the most relevant live status of any addon
    if (live.text.empty()) return line;
    std::string who = live.addon;
    if (manager) for (const auto& addon : manager->GetAddons()) if (addon.id == live.addon) who = addon.GetDisplayName();
    return WithLiveStatus(line, who, live.text);
}

void RenderTabs(AddonManager* manager, bool& bringAddonsForward) {
    if (!ImGui::BeginTabBar("##MainTabs", ImGuiTabBarFlags_None)) return;

    if (ImGui::BeginTabItem("Addons", nullptr, bringAddonsForward ? ImGuiTabItemFlags_SetSelected : 0)) {
        bringAddonsForward = false;
        ImGui::Dummy(ImVec2(0, 5));
        RenderTabAddons(manager);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Features")) { RenderTabFeatures(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Performance")) { RenderTabPerformance(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Settings")) { RenderTabSettings(manager); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Logs")) {
        ImGui::Dummy(ImVec2(0, 5));
        RenderTabLogs();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("About")) { RenderTabAbout(); ImGui::EndTabItem(); }
    ImGui::EndTabBar();
}

} // namespace

void RenderMainFrame(AddonManager* manager, bool& bringAddonsForward) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##Main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // the header, fixed; then everything but the status bar along the bottom, which scrolls when a tab is long
    widgets::HeaderBar();
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::BeginChild("##content", ImVec2(0, -widgets::StatusBarHeight() - 2.0f), false, ImGuiWindowFlags_NoBackground);
    RenderTabs(manager, bringAddonsForward);
    ImGui::EndChild();

    widgets::StatusBar(BuildStatusLine(manager));
    widgets::UpdateOffer();   // over everything: the offer to update, when the check finds a newer release
    ImGui::End();
}

} // namespace window
} // namespace eam
