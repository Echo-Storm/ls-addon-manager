#include "tab_features.h"
#include "../gui_scale.h"
#include "../gui_style.h"
#include "../widgets/toggle_switch.h"
#include "../widgets/tooltip.h"
#include "../../features/features.h"
#include "imgui.h"
#include "eam/widgets.h"
#include <string>

namespace eam {

namespace {

// A small outlined label in the warning colour, like the one on an addon's card.
void Chip(const char* text) {
    using namespace eam::ui::theme;
    const ImVec2 at = ImGui::GetCursorScreenPos(), size = ImGui::CalcTextSize(text);
    const float padX = S(6), padY = S(1);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRect(at, ImVec2(at.x + size.x + padX * 2, at.y + size.y + padY * 2), U(kWarn), S(3.0f), 0, 1.0f);
    draw->AddText(ImVec2(at.x + padX, at.y + padY), U(kWarn), text);
    ImGui::Dummy(ImVec2(size.x + padX * 2, size.y + padY * 2));
}

void Card(int index) {
    using namespace eam::ui::theme;
    const features::Info& info = features::At(index);
    const bool on = features::IsOn(index);

    ImGui::PushID(index);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, V(on ? kRowHover : kPanel));
    ImGui::PushStyleColor(ImGuiCol_Border, V(on ? kAccentDim : kBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(4.0f));
    ImGui::BeginChild("card", ImVec2(-1, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);

    const float switchWidth = ImGui::GetFrameHeight() * 1.8f * 0.8f;
    const float rowTop = ImGui::GetCursorPosY();
    const float icon = S(36.0f);
    const float textWidth = ImGui::GetContentRegionAvail().x - switchWidth - icon - S(36);

    {   // the feature's icon, on a tile like an addon's
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        draw->AddRectFilled(at, ImVec2(at.x + icon, at.y + icon), U(kPanelAlt), S(4.0f));
        draw->AddRect(at, ImVec2(at.x + icon, at.y + icon), U(on ? kAccentDim : kBorderBright), S(4.0f));
        const std::string id = info.id;   // one shape for each: a keyboard, a monitor (windowed mode), a video camera (the recorder), a gauge (the GPU limit)
        const char* shape = id.find("ReShade") != std::string::npos ? eam::ui::icons::kKeyboard : id == "Recorder" ? eam::ui::icons::kVideo : id == "GpuLimit" ? eam::ui::icons::kGauge : eam::ui::icons::kMonitor;
        eam::ui::svg::Draw(draw, shape, ImVec2(at.x + icon * 0.19f, at.y + icon * 0.19f), icon * 0.62f, U(on ? kAccent : kMuted), 1.7f);
        ImGui::Dummy(ImVec2(icon, icon));
        ImGui::SameLine(0, S(12));
    }

    ImGui::BeginGroup();
    if (ImFont* title = TitleFont()) ImGui::PushFont(title, 0.0f);
    ImGui::Text("%s", info.title);
    if (TitleFont()) ImGui::PopFont();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textWidth);
    ImGui::TextDisabled("%s", info.summary);
    ImGui::PopTextWrapPos();
    if (features::NeedsRestart(index)) Chip("RESTART LOSSLESS SCALING TO APPLY");
    const std::string status = features::Status(index);
    if (!status.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, V(kAccent));
        ImGui::TextUnformatted(status.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();

    ImGui::SameLine(ImGui::GetWindowWidth() - switchWidth - S(15));
    ImGui::SetCursorPosY(rowTop + S(2));
    bool wanted = on;
    if (widgets::ToggleSwitch("##on", &wanted)) features::SetOn(index, wanted);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) widgets::Tip(info.tooltip);

    if (on) {
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, S(2)));
        features::RenderOptions(index);
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    ImGui::PopID();
}

} // namespace

void RenderTabFeatures() {
    ImGui::Dummy(ImVec2(0, S(5)));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Built into the manager. Switch on what you need; each one shows its few options once it is on.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, S(6)));

    for (int i = 0; i < features::Count(); ++i) {
        Card(i);
        ImGui::Dummy(ImVec2(0, S(4)));
    }
}

} // namespace eam
