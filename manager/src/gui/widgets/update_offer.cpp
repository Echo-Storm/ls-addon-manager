#include "update_offer.h"
#include "toast.h"
#include "../../update/update_check.h"
#include "../../update/updater.h"
#include "../gui_scale.h"
#include "imgui.h"
#include <eam/icons.h>
#include <eam/version.h>
#include <eam/widgets.h>
#include <windows.h>
#include <shellapi.h>
#include <string>

namespace eam {
namespace widgets {

namespace {
bool g_requested = false;          // the About tab asked for it
int64_t g_offeredAt = 0;           // the check the offer last opened for by itself ("Not now" lasts until the next daily check)
constexpr const char* kPopup = "Update available###update_offer";

std::wstring LosslessScalingFolder() {   // the manager lives in LosslessScaling.exe: its folder
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    const size_t cut = dir.find_last_of(L"\\/");
    return cut == std::wstring::npos ? dir : dir.substr(0, cut);
}

void Wrapped(const char* text, bool disabled = false) {
    if (disabled) ImGui::TextDisabled("%s", text); else ImGui::TextUnformatted(text);
}
} // namespace

void OpenUpdateOffer() { g_requested = true; }

void UpdateOffer() {
    const update::Status st = update::Current();
    const bool available = st.state == update::State::Available && !st.latest.empty();
    if (available && (g_requested || (g_offeredAt != st.checkedAt && !update::Skipped(st.latest)))) {
        g_offeredAt = st.checkedAt;
        ImGui::OpenPopup(kPopup);
    }
    g_requested = false;

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.0f, 0));
    if (!ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 33.0f);
    const update::Progress pr = update::CurrentProgress();
    const bool forThis = pr.version == st.latest;
    const update::Phase step = forThis ? pr.step : update::Phase::Idle;
    auto close = [] { ImGui::CloseCurrentPopup(); };

    if (step == update::Phase::Downloading || step == update::Phase::Checking) {
        ImGui::Text("Downloading LS Addon Manager %s...", st.latest.c_str());
        const float mb = static_cast<float>(pr.done) / 1048576.0f, of = static_cast<float>(pr.total) / 1048576.0f;
        char label[64];
        if (pr.total) snprintf(label, sizeof label, "%.0f of %.0f MB", mb, of); else snprintf(label, sizeof label, "%.0f MB", mb);
        ImGui::ProgressBar(pr.total ? mb / of : -1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1, 0), label);
        Wrapped("Then it is checked against GitHub's fingerprint of the release before anything is used.", true);
        ImGui::Dummy(ImVec2(0, S(4)));
        if (ui::Button("Cancel", ui::icons::kClose)) update::CancelDownload();
    } else if (step == update::Phase::Ready) {
        ImGui::Text("LS Addon Manager %s is downloaded and checked.", st.latest.c_str());
        ImGui::Dummy(ImVec2(0, S(2)));
        Wrapped("To install it, Lossless Scaling has to close: Setup waits for that, updates it (with backups, as always) and starts it again. "
                "Your settings carry over.");
        ImGui::Dummy(ImVec2(0, S(6)));
        if (ui::Button("Install now", ui::icons::kDownload, ui::ButtonKind::Primary)) {
            std::string error;
            if (!update::StartSetup(LosslessScalingFolder(), error)) ToastShow("Setup could not be started: " + error, ToastType::Error, 8.0f);
        }
        ImGui::SameLine();
        if (ui::Button("Later", ui::icons::kClose)) close();   // it stays downloaded: the About tab's button brings this back
    } else if (step == update::Phase::Handed) {
        ImGui::TextUnformatted("Setup is waiting for Lossless Scaling to close.");
        ImGui::Dummy(ImVec2(0, S(2)));
        Wrapped("Close Lossless Scaling: its window, or right-click its icon next to the clock and choose Exit. Setup then updates it and starts it again.");
        ImGui::Dummy(ImVec2(0, S(6)));
        if (ui::Button("OK", ui::icons::kCheck, ui::ButtonKind::Primary)) close();
    } else {   // the offer (or the offer again after a failed or cancelled try)
        ImGui::Text("LS Addon Manager %s is available.", st.latest.c_str());
        ImGui::TextDisabled("You have %s.", EAM_VERSION_STRING);
        if (forThis && pr.step == update::Phase::Failed && pr.error != "cancelled") {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::theme::V(ui::theme::kWarn));
            ImGui::TextWrapped("The download did not work: %s.", pr.error.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, S(2)));
        if (ImGui::SmallButton("What's new")) ShellExecuteA(nullptr, "open", st.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine(); ImGui::TextDisabled("(the release page)");
        ImGui::Dummy(ImVec2(0, S(6)));
        const bool canDownload = !st.zipUrl.empty();
        if (!canDownload) ImGui::BeginDisabled();
        if (ui::Button("Download and install", ui::icons::kDownload, ui::ButtonKind::Primary)) { update::ForgetDownload(); update::StartDownload(st); }
        if (!canDownload) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ui::Button("Not now", ui::icons::kClose)) close();
        ImGui::SameLine();
        if (ui::Button("Don't ask again for this release")) { update::Skip(st.latest); close(); }
        if (!canDownload) Wrapped("This release has no download attached: use its page.", true);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndPopup();
}

} // namespace widgets
} // namespace eam
