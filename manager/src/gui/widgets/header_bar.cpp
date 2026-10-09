#include "header_bar.h"
#include "tooltip.h"
#include "../gui_scale.h"
#include "../gui_style.h"
#include "update_offer.h"
#include "../../update/update_check.h"
#include "../../host/gpu_stats.h"
#include "../../host/system_stats.h"
#include "eam/icons.h"
#include "eam/version.h"
#include "eam/widgets.h"
#include "imgui.h"
#include <cstdio>
#include <string>
#include <vector>

namespace eam {
namespace widgets {

namespace {

using namespace eam::ui::theme;

// A memory figure's colour by how full it is: green, amber past 80%, red past 93%.
const float* FullnessColour(double used, double total) {
    const double f = total > 0 ? used / total : 0;
    return f < 0.80 ? kAccent : f < 0.93 ? kWarn : kDanger;
}
// A load figure's colour: plain text, amber when pinned (a game at its limit is normal; this only says there is no room left).
const float* LoadColour(double percent) { return percent < 95.0 ? kText : kWarn; }

struct Reading { const char* caption; std::string value; const float* colour; };

std::string Gigabytes(uint64_t usedMB, uint64_t totalMB) {
    char text[48];
    snprintf(text, sizeof text, "%.1f / %.0f GB", usedMB / 1024.0, totalMB / 1024.0);
    return text;
}

// The logo: the three stacked tiles of the About tab, small.
void Logo(ImDrawList* dl, ImVec2 at, float size) {
    eam::ui::svg::Draw(dl, eam::ui::icons::kEchoBack, at, size, U(kAccentDim), 1.8f);
    eam::ui::svg::Draw(dl, eam::ui::icons::kEchoMid, at, size, U(kAccent), 1.8f);
    eam::ui::svg::Draw(dl, eam::ui::icons::kEchoFront, at, size, U(kAccentHot), 1.8f, U(kAccent));
}

} // namespace

float HeaderBarHeight() { return S(50.0f); }

void HeaderBar() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = HeaderBarHeight();
    ImFont* title = TitleFont();
    const float base = ImGui::GetStyle().FontSizeBase;

    // left: logo, name, version
    const float logo = S(28.0f);
    Logo(dl, ImVec2(origin.x, origin.y + (height - logo) * 0.5f - S(2)), logo);
    float x = origin.x + logo + S(12);
    const float titleSize = base * 1.25f * ImGui::GetStyle().FontScaleDpi;
    const float titleY = origin.y + (height - titleSize) * 0.5f - S(2);
    dl->AddText(title ? title : ImGui::GetFont(), titleSize, ImVec2(x, titleY), U(kText), "Addon Manager");
    x += (title ? title : ImGui::GetFont())->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Addon Manager").x + S(8);
    const float smallSize = ImGui::GetFontSize() * 0.92f;
    const float smallY = titleY + titleSize - smallSize - S(1);
    dl->AddText(ImGui::GetFont(), smallSize, ImVec2(x, smallY), U(kMuted), "for Lossless Scaling");
    x += ImGui::GetFont()->CalcTextSizeA(smallSize, FLT_MAX, 0.0f, "for Lossless Scaling").x + S(10);
    const std::string version = std::string("v") + EAM_VERSION_STRING;
    const ImVec2 vs = ImGui::GetFont()->CalcTextSizeA(smallSize * 0.92f, FLT_MAX, 0.0f, version.c_str());
    const ImVec2 chip(x, smallY - S(1));
    dl->AddRect(chip, ImVec2(chip.x + vs.x + S(10), chip.y + vs.y + S(3)), U(kAccentDim), S(3.0f));
    dl->AddText(ImGui::GetFont(), smallSize * 0.92f, ImVec2(chip.x + S(5), chip.y + S(1.5f)), U(kAccent), version.c_str());
    float leftEnd = chip.x + vs.x + S(10);
    {   // an update is waiting (also after "Not now" or "Don't ask again"): a badge that opens the offer
        const update::Status us = update::Current();
        if (us.state == update::State::Available && !us.latest.empty()) {
            const std::string label = "Update " + us.latest + " available";
            const float fs = smallSize * 0.92f;
            const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, label.c_str());
            const ImVec2 p0(leftEnd + S(10), chip.y - S(1)), p1(p0.x + ts.x + S(14), p0.y + ts.y + S(5));
            const ImVec2 saved = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(p0);
            const bool clicked = ImGui::InvisibleButton("##update_badge", ImVec2(p1.x - p0.x, p1.y - p0.y));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::SetCursorScreenPos(saved);
            dl->AddRectFilled(p0, p1, hovered ? U(kAccent) : U(kAccentDim), S(3.0f));
            dl->AddText(ImGui::GetFont(), fs, ImVec2(p0.x + S(7), p0.y + S(2.5f)), hovered ? U(kBg) : U(kText), label.c_str());
            if (hovered) Tip("A newer version is available. Click for what is new and to install it.");
            if (clicked) OpenUpdateOffer();
            leftEnd = p1.x;
        }
    }

    // right: the machine, right to left
    GpuStats::Instance().Wanted();   // keeps the card's sampler running while the window is drawn
    const GpuStats::Snapshot g = GpuStats::Instance().Get();
    const SystemStats::Snapshot s = SystemStats::Instance().Get();
    std::vector<Reading> readings;
    if (g.ok) {
        char load[16]; snprintf(load, sizeof load, "%u%%", g.utilGpu);
        readings.push_back({ "GPU", load, LoadColour(g.utilGpu) });
        if (g.vramTotalMB) readings.push_back({ "VRAM", Gigabytes(g.vramUsedMB, g.vramTotalMB), FullnessColour((double)g.vramUsedMB, (double)g.vramTotalMB) });
    }
    if (s.ok) {
        readings.push_back({ "RAM", Gigabytes(s.ramUsedMB, s.ramTotalMB), FullnessColour((double)s.ramUsedMB, (double)s.ramTotalMB) });
        char cpu[16]; snprintf(cpu, sizeof cpu, "%.0f%%", s.cpuPercent);
        readings.push_back({ "CPU", cpu, LoadColour(s.cpuPercent) });
    }
    const float captionSize = ImGui::GetFontSize() * 0.78f, valueSize = ImGui::GetFontSize() * 1.02f;
    const float gap = S(22);
    float right = origin.x + width;
    float readoutLeft = right;
    for (size_t i = readings.size(); i-- > 0;) {
        const Reading& r = readings[i];
        ImFont* vf = title ? title : ImGui::GetFont();
        const float vw = vf->CalcTextSizeA(valueSize, FLT_MAX, 0.0f, r.value.c_str()).x;
        const float cw = ImGui::GetFont()->CalcTextSizeA(captionSize, FLT_MAX, 0.0f, r.caption).x;
        const float w = vw > cw ? vw : cw;
        const float left = right - w;
        if (left < leftEnd + S(20)) break;   // a narrow window: the readings that do not fit are left out
        const float top = origin.y + (height - (captionSize + valueSize + S(2))) * 0.5f - S(2);
        dl->AddText(ImGui::GetFont(), captionSize, ImVec2(right - cw, top), U(kMuted), r.caption);
        dl->AddText(vf, valueSize, ImVec2(right - vw, top + captionSize + S(2)), U(r.colour), r.value.c_str());
        readoutLeft = left;
        right = left - gap;
        if (i > 0) dl->AddLine(ImVec2(right + gap * 0.5f, top + S(2)), ImVec2(right + gap * 0.5f, top + captionSize + valueSize), U(kBorderBright), 1.0f);
    }

    // the line under the band
    dl->AddLine(ImVec2(origin.x, origin.y + height - 1), ImVec2(origin.x + width, origin.y + height - 1), U(kBorder), 1.0f);
    ImGui::Dummy(ImVec2(width, height));

    // the details, when the pointer rests on the readout
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (!readings.empty() && ImGui::IsWindowHovered() && mouse.x >= readoutLeft && mouse.x <= origin.x + width && mouse.y >= origin.y && mouse.y <= origin.y + height) {
        std::string tip;
        char line[160];
        if (g.ok) {
            snprintf(line, sizeof line, "%s\n%u%% load, %u C, %.0f of %.0f W\n%u MHz core, %u MHz memory\n%s of video memory in use\n\n", g.name.c_str(), g.utilGpu, g.tempC,
                     g.powerW, g.powerLimitW, g.clockGraphics, g.clockMem, Gigabytes(g.vramUsedMB, g.vramTotalMB).c_str());
            tip += line;
        } else if (!g.why.empty()) {
            tip += "Graphics card: " + g.why + "\n\n";
        }
        if (s.ok) {
            snprintf(line, sizeof line, "%s of system memory in use\nProcessor %.0f%% busy (all cores)", Gigabytes(s.ramUsedMB, s.ramTotalMB).c_str(), s.cpuPercent);
            tip += line;
        }
        tip += "\n\nThe Performance tab has the history and what it means.";
        Tip(tip.c_str());
    }
}

} // namespace widgets
} // namespace eam
