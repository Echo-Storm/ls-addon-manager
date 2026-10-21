#include "addon_card.h"
#include "toggle_switch.h"
#include "tooltip.h"
#include "../gui_scale.h"
#include "../gui_style.h"
#include "../../host/metrics.h"
#include "eam/widgets.h"
#include "eam/version.h"
#include <cstdio>
#include <string>

namespace eam {
namespace widgets {

namespace {

const ImU32 kRed = IM_COL32(208, 80, 80, 255);
const ImU32 kGrey = IM_COL32(102, 102, 102, 255);
const ImU32 kChipRed = IM_COL32(208, 128, 128, 255);

// The dot on the icon's corner: red for trouble, amber when it is on but unchecked, green when it is running, grey otherwise.
ImU32 DotColour(const AddonInfo& a) {
    using namespace eam::ui::theme;
    if (a.faulted || a.security == SecurityVerdict::Tampered) return kRed;
    if (a.security == SecurityVerdict::Unknown && a.enabled) return U(kWarn);
    if (a.IsLoaded()) return U(kAccent);
    return kGrey;
}

// The small outlined label beside the name, only for states that need attention (the switch already says on or off).
struct Chip { const char* text = nullptr; ImU32 colour = 0; };

Chip ChipFor(const AddonInfo& a) {
    using namespace eam::ui::theme;
    if (a.manifest.wip) return { "WIP", U(kWarn) };
    if (a.faulted) return { "ERROR", kChipRed };
    if (a.security == SecurityVerdict::Tampered) return { "TAMPERED", kChipRed };
    if (a.OlderBuild(EAM_VERSION_STRING)) return { "OLD BUILD", U(kWarn) };   // (its DLL is older than the manager: see the addon's page)
    if (a.enabled && !a.IsLoaded()) return { a.RequiresRestart() ? "RESTART TO APPLY" : "NOT LOADED", U(kWarn) };
    return {};
}

const float* LiveColour(int level) {
    using namespace eam::ui::theme;
    switch (level) {
        case 1: return kAccent;
        case 2: return kWarn;
        case 3: return kDanger;
        default: return kMuted;
    }
}

} // namespace

// The addon's own icon (a picture, or the shapes of its icon.svg drawn in the theme's colours), or a package glyph when it ships none;
// on a dark tile, so every icon sits the same way.
void DrawAddonIcon(ImDrawList* draw, ImVec2 at, float size, const AddonInfo& a, bool lit) {
    using namespace eam::ui::theme;
    const ImVec2 bottomRight(at.x + size, at.y + size);
    if (a.iconTexture) {
        draw->AddImageRounded((ImTextureID)a.iconTexture, at, bottomRight, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255), S(4.0f));
        return;
    }
    draw->AddRectFilled(at, bottomRight, U(kPanelAlt), S(4.0f));
    draw->AddRect(at, bottomRight, U(lit ? kAccentDim : kBorderBright), S(4.0f), 0, 1.0f);
    const float glyph = size * 0.62f, inset = (size - glyph) * 0.5f;
    const ImU32 colour = U(lit ? kAccent : kMuted);
    if (!a.iconSvg.empty())
        for (const std::string& d : a.iconSvg) eam::ui::svg::Draw(draw, d.c_str(), ImVec2(at.x + inset, at.y + inset), glyph, colour, 1.7f, 0, a.iconSvgView);
    else
        eam::ui::svg::Draw(draw, eam::ui::icons::kPackage, ImVec2(at.x + inset, at.y + inset), glyph, colour, 1.7f);
}

namespace {

[[maybe_unused]] void DrawChip(const Chip& chip) {
    ImGui::SameLine(0, S(8));
    const ImVec2 at = ImGui::GetCursorScreenPos(), text = ImGui::CalcTextSize(chip.text);
    const float padX = S(6), padY = S(1);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRect(at, ImVec2(at.x + text.x + padX * 2, at.y + text.y + padY * 2), chip.colour, S(3.0f), 0, 1.0f);
    draw->AddText(ImVec2(at.x + padX, at.y + padY), chip.colour, chip.text);
    ImGui::Dummy(ImVec2(text.x + padX * 2, text.y + padY * 2));
}

std::string SwitchTooltip(const AddonInfo& a) {
    std::string tip = a.GetDisplayName() + "\n\n";
    tip += a.manifest.description.empty() ? std::string("No description.") : a.manifest.description;
    tip += "\n\n";
    if (a.manifest.wip) tip += "Work in progress: it cannot be switched on yet.";
    else tip += a.enabled ? "On: switch it off to stop using this addon." : "Off: switch it on to use this addon.";
    if (a.RequiresRestart()) tip += "\nChanging this takes effect the next time Lossless Scaling starts.";
    if (a.OlderBuild(EAM_VERSION_STRING)) tip += "\n\nIts file is version " + a.dllVersion + ", older than the manager (" EAM_VERSION_STRING "): a test build or an old file. Close Lossless Scaling and run Setup (Reinstall) to put the release's file back.";
    return tip;
}

} // namespace

// One row of the sidebar: the icon (with its state dot), the name, and under it what needs saying (a problem, the addon's live status, or
// its version and author), with the switch at the right. The selected row is tinted and marked with a bar of the accent colour.
bool AddonCard(AddonInfo& addon, int index, bool isSelected, bool* toggled) {
    using namespace eam::ui::theme;
    bool clicked = false;
    if (toggled) *toggled = false;
    ImGui::PushID(index);

    const float rowHeight = S(54.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 end(at.x + width, at.y + rowHeight);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(at, end);
    if (isSelected || hovered) draw->AddRectFilled(at, end, U(isSelected ? kRowHover : kPanelAlt), S(4.0f));
    if (isSelected) draw->AddRectFilled(at, ImVec2(at.x + S(3), end.y), U(kAccent), S(1.5f));

    // icon and state dot
    const float icon = S(32.0f), pad = S(10.0f);
    const ImVec2 iconAt(at.x + pad, at.y + (rowHeight - icon) * 0.5f);
    DrawAddonIcon(draw, iconAt, icon, addon, addon.enabled);
    const ImVec2 dot(iconAt.x + icon - S(3), iconAt.y + icon - S(3));
    draw->AddCircleFilled(dot, S(4.5f), DotColour(addon));
    draw->AddCircle(dot, S(4.5f), U(isSelected ? kRowHover : kBg), 12, S(1.5f));

    // the switch, at the right
    const float switchScale = 0.85f;
    const float switchH = ImGui::GetFrameHeight() * 0.8f * switchScale, switchW = switchH * 1.8f;
    const float textLeft = iconAt.x + icon + S(10), textRight = end.x - switchW - S(14);

    // the name, and the line under it
    ImFont* title = TitleFont();
    const float nameSize = ImGui::GetFontSize(), smallSize = ImGui::GetFontSize() * 0.86f;
    const float nameY = at.y + rowHeight * 0.5f - nameSize - S(1);
    const float lineY = at.y + rowHeight * 0.5f + S(2);
    const ImVec4 clip(textLeft, at.y, textRight, end.y);
    draw->AddText(title ? title : ImGui::GetFont(), nameSize, ImVec2(textLeft, nameY), U(addon.enabled ? kText : kMuted), addon.GetDisplayName().c_str(), nullptr, 0.0f, &clip);
    std::string second; ImU32 secondColour = U(kMuted);
    const Metrics::Status live = Metrics::Instance().GetStatus(addon.id);   // the addon's own one-line status, if it reports one
    if (const Chip chip = ChipFor(addon); chip.text) { second = chip.text; secondColour = chip.colour; }
    else if (!live.text.empty() && addon.enabled) { second = live.text; secondColour = U(LiveColour(live.level)); }
    else second = "v" + addon.GetDisplayVersion() + "  \xc2\xb7  " + addon.GetDisplayAuthor();
    std::string shown = second;   // cut to fit, with an ellipsis
    const float room = textRight - textLeft;
    while (shown.size() > 4 && ImGui::GetFont()->CalcTextSizeA(smallSize, FLT_MAX, 0.0f, shown.c_str()).x > room) shown = shown.substr(0, shown.size() - 5) + "...";
    draw->AddText(ImGui::GetFont(), smallSize, ImVec2(textLeft, lineY), secondColour, shown.c_str(), nullptr, 0.0f, &clip);

    // the whole row selects; the switch sits on top of it
    ImGui::SetCursorScreenPos(at);
    ImGui::InvisibleButton("##row", ImVec2(width - switchW - S(12), rowHeight));
    if (ImGui::IsItemClicked()) clicked = true;
    if (shown != second) Tip(second.c_str());
    ImGui::SetCursorScreenPos(ImVec2(end.x - switchW - S(10), at.y + (rowHeight - switchH) * 0.5f));
    char switchId[32];
    snprintf(switchId, sizeof switchId, "##toggle_%d", index);
    bool on = addon.enabled;
    if (addon.manifest.wip) ImGui::BeginDisabled();
    if (ToggleSwitch(switchId, &on, switchScale)) {
        addon.enabled = on;
        if (toggled) *toggled = true;
        clicked = true;
    }
    if (addon.manifest.wip) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) Tip(SwitchTooltip(addon).c_str());

    ImGui::SetCursorScreenPos(ImVec2(at.x, end.y));
    ImGui::Dummy(ImVec2(width, 0));
    ImGui::PopID();
    return clicked;
}

} // namespace widgets
} // namespace eam
