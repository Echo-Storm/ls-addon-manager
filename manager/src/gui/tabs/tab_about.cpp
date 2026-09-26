#include "../widgets/update_offer.h"
#include "tab_about.h"
#include "../gui_scale.h"
#include "../gui_style.h"
#include "../widgets/status_bar.h"
#include "../../update/update_check.h"
#include "../../../sdk/include/eam/version.h"
#include "imgui.h"
#include "eam/widgets.h"
#include <cstdio>
#include <windows.h>
#include <shellapi.h>

namespace eam {

static void OpenUrl(const char* url) { ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL); }

static void CenteredText(const char* text, bool muted = false) {
    const float w = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w) * 0.5f + ImGui::GetCursorPosX());
    if (muted) ImGui::TextDisabled("%s", text); else ImGui::TextUnformatted(text);
}

void RenderTabAbout() {
    ImGui::Dummy(ImVec2(0, S(12)));

    // logo: the Echo mark on a dark tile, drawn as vectors (crisp at any scale)
    {
        const float s = ImGui::GetFontSize() * 4.2f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - s) * 0.5f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), eam::ui::theme::U(eam::ui::theme::kPanel), s * 0.22f);
        dl->AddRect(p, ImVec2(p.x + s, p.y + s), eam::ui::theme::U(eam::ui::theme::kAccentDim), s * 0.22f, 0, 1.5f);
        const float m = s * 0.06f;
        const float in = s - 2 * m;
        eam::ui::svg::Draw(dl, eam::ui::icons::kEchoBack, ImVec2(p.x + m, p.y + m), in, eam::ui::theme::U(eam::ui::theme::kAccentDim), 1.8f);
        eam::ui::svg::Draw(dl, eam::ui::icons::kEchoMid, ImVec2(p.x + m, p.y + m), in, eam::ui::theme::U(eam::ui::theme::kAccent), 1.8f);
        eam::ui::svg::Draw(dl, eam::ui::icons::kEchoFront, ImVec2(p.x + m, p.y + m), in, eam::ui::theme::U(eam::ui::theme::kAccentHot), 1.8f, eam::ui::theme::U(eam::ui::theme::kAccent));
        ImGui::Dummy(ImVec2(s, s));
        ImGui::Dummy(ImVec2(0, S(6)));
    }

    // title block
    CenteredText(EAM_PRODUCT_NAME);
    char versionStr[64];
    snprintf(versionStr, sizeof(versionStr), "v%s", EAM_VERSION_STRING);
    CenteredText(versionStr, true);
    {   // is there a newer one? (the daily check, or the button)
        const update::Status st = update::Current();
        const std::string line = update::DescribeStatus(st);
        if (st.state == update::State::Available) {
            ImGui::PushStyleColor(ImGuiCol_Text, eam::ui::theme::V(eam::ui::theme::kAccent));
            CenteredText(line.c_str());
            ImGui::PopStyleColor();
            const float bw1 = ImGui::CalcTextSize("Download and install").x + ImGui::GetFontSize() * 4.0f;
            const float bw2 = ImGui::CalcTextSize("Open the download page").x + ImGui::GetFontSize() * 4.0f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - bw1 - bw2 - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
            if (eam::ui::Button("Download and install", eam::ui::icons::kDownload, eam::ui::ButtonKind::Primary, ImVec2(bw1, 0))) widgets::OpenUpdateOffer();
            ImGui::SameLine();
            if (eam::ui::Button("Open the download page", eam::ui::icons::kExternal, eam::ui::ButtonKind::Flat, ImVec2(bw2, 0))) OpenUrl(st.url.c_str());
        } else {
            if (st.state != update::State::Idle) CenteredText(line.c_str(), true);
            const bool checking = st.state == update::State::Checking;
            const float bw2 = ImGui::CalcTextSize("Check for updates").x + ImGui::GetFontSize() * 3.0f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - bw2) * 0.5f);
            if (checking) ImGui::BeginDisabled();
            if (eam::ui::Button("Check for updates", eam::ui::icons::kCheck, eam::ui::ButtonKind::Flat, ImVec2(bw2, 0))) update::StartCheckAsync();
            if (checking) ImGui::EndDisabled();
        }
    }
    ImGui::Dummy(ImVec2(0, S(6)));
    CenteredText("Lossless Scaling, extended: neural rendering, DLSS and FSR upscaling, and anything else an addon adds.", true);

    // support
    ImGui::Dummy(ImVec2(0, S(16)));
    const float bw = ImGui::CalcTextSize("Support on Ko-fi").x + ImGui::GetFontSize() * 4.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - bw) * 0.5f);
    if (eam::ui::Button("Support on Ko-fi", eam::ui::icons::kHeart, eam::ui::ButtonKind::Primary, ImVec2(bw, ImGui::GetFrameHeight() * 1.25f)))
        widgets::OpenKofi();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("If this is useful, a Ko-fi helps a lot. Opens your browser.");

    // what comes with it: the three plugins and what is built into the manager
    ImGui::Dummy(ImVec2(0, S(18)));
    eam::ui::SectionLabel("What comes with it");
    {
        struct Item { const char* icon; const char* name; const char* what; };
        const Item items[] = {
            { eam::ui::icons::kSparkles, "DLSS 5 Neural Rendering",
              "NVIDIA's neural rendering model on every frame Lossless Scaling shows, real and generated: a new look for any game, with its own "
              "motion measurement, saved looks and a look per game. NVIDIA RTX; needs your own copy of the model file." },
            { eam::ui::icons::kUpscale, "DLSS Upscaler",
              "NVIDIA DLSS in place of Lossless Scaling's NIS scaler: real upscaling (or DLAA at the screen's own size) for games that never had "
              "it, with motion measured from the frames. NVIDIA RTX." },
            { eam::ui::icons::kUpscaleFast, "FSR Upscaler",
              "AMD FSR (3.1, or FSR 4) in the same place, on any DirectX 12 graphics card. Both upscalers take 4:3 games too, keep their settings per game, and "
              "have Stability and Edge smoothing for older games without anti-aliasing." },
            { eam::ui::icons::kSliders, "Built into the manager",
              "ReShade input passthrough, windowed games and a second monitor, a Performance tab that says what limits the frame rate, a tray "
              "icon and hotkey, a one-file installer that repairs itself after a Lossless Scaling update, and an update check." },
        };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float icon = S(30.0f);
        for (const Item& it : items) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(at, ImVec2(at.x + icon, at.y + icon), eam::ui::theme::U(eam::ui::theme::kPanelAlt), S(4.0f));
            dl->AddRect(at, ImVec2(at.x + icon, at.y + icon), eam::ui::theme::U(eam::ui::theme::kAccentDim), S(4.0f));
            eam::ui::svg::Draw(dl, it.icon, ImVec2(at.x + icon * 0.19f, at.y + icon * 0.19f), icon * 0.62f, eam::ui::theme::U(eam::ui::theme::kAccent), 1.7f);
            ImGui::Dummy(ImVec2(icon, icon));
            ImGui::SameLine(0, S(12));
            ImGui::BeginGroup();
            if (ImFont* title = TitleFont()) ImGui::PushFont(title, 0.0f);
            ImGui::TextUnformatted(it.name);
            if (TitleFont()) ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextWrapped("%s", it.what);
            ImGui::PopStyleColor();
            ImGui::EndGroup();
            ImGui::Dummy(ImVec2(0, S(6)));
        }
    }

    ImGui::Dummy(ImVec2(0, S(14)));
    eam::ui::SectionLabel("Build");
    ImGui::TextDisabled("Addon API");   ImGui::SameLine(S(120)); ImGui::Text("v%s", EAM_API_VERSION_STRING);
    ImGui::TextDisabled("ImGui");        ImGui::SameLine(S(120)); ImGui::Text("%s", ImGui::GetVersion());
    ImGui::TextDisabled("Built");        ImGui::SameLine(S(120)); ImGui::Text("%s %s", __DATE__, __TIME__);

    ImGui::Dummy(ImVec2(0, S(14)));
    eam::ui::SectionLabel("Good to know");
    ImGui::TextWrapped("Closed this window? It is still running in the notification area (tray). Click the icon, or use the hotkey set in "
                       "Settings, to open it again.");
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::TextWrapped("Hold Ctrl and scroll over any slider to fine-tune it; double-click a slider to put it back to its default.");

    ImGui::Dummy(ImVec2(0, S(14)));
    eam::ui::SectionLabel("Thanks");
    ImGui::TextWrapped("This began as LosslessProxy by FrankBarretta (MIT), and we are grateful for it: the idea of a proxy Lossless.dll with "
                       "addons, the addon interface (which is why LosslessProxy's addons still load here), and the ReShade and Windowed features, "
                       "which began as addons of theirs and are built in now. The code has since been rewritten.");
    ImGui::Dummy(ImVec2(0, S(4)));
    if (eam::ui::Button("The original project", eam::ui::icons::kExternal, eam::ui::ButtonKind::Flat)) OpenUrl("https://github.com/FrankBarretta/LosslessProxy");
    ImGui::Dummy(ImVec2(0, S(10)));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("NVIDIA DLSS: the Neural Rendering addon uses NVIDIA DLSS technology and includes NVIDIA's NGX SDK and DLSS runtime under "
                       "NVIDIA's RTX SDKs licence (NVIDIA-LICENSE.txt in its folder). NVIDIA, the NVIDIA logo and DLSS are trademarks of NVIDIA "
                       "Corporation. This project is not affiliated with or endorsed by NVIDIA.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("AMD FidelityFX: the FSR Upscaler includes AMD's FidelityFX runtime (MIT, AMD-FidelityFX-LICENSE.txt in its folder). AMD, "
                       "FidelityFX and FSR are trademarks of Advanced Micro Devices, Inc.; this project is not affiliated with or endorsed by AMD.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::TextDisabled("Also: Dear ImGui and nlohmann/json (MIT), stb_image (public domain), MinHook (BSD, used by Windowed mode).");
    ImGui::TextDisabled("Icon shapes are drawn in the manner of the Lucide set (ISC).");
    ImGui::TextDisabled("Lossless Scaling belongs to its author; this is an unofficial add-on for it.");

    ImGui::Dummy(ImVec2(0, S(14)));
    eam::ui::SectionLabel("For addon authors");
    ImGui::TextDisabled("Include the SDK headers (addon_sdk.h). widgets.h gives an addon this same look: the palette,");
    ImGui::TextDisabled("sliders with defaults, section headers, buttons and SVG icons.");
}

} // namespace eam
