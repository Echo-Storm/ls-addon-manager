// The addon's settings panel, drawn by the manager in its window (the window's thread). It works on a copy of the settings and hands the copy
// back (Commit) when something changed. The sections start closed; the person opens what they need.
#include "addon/state.h"
#include "addon/present_hook.h"
#include "addon/screenshot.h"
#include "addon/hud_editor.h"
#include "imgui.h"
#include <eam/widgets.h>
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace nr {

// Wrapped tooltip for the widget just submitted, after a short hover delay.
static void Tip(const char* text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) return;
    const char* hint = eam::ui::SliderHint();   // sliders add "default, double-click resets, Ctrl+scroll fine-tunes"
    ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f); ImGui::TextUnformatted(text);
    if (hint) { ImGui::Dummy(ImVec2(0, 2)); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]); ImGui::TextUnformatted(hint); ImGui::PopStyleColor(); }
    ImGui::PopTextWrapPos(); ImGui::EndTooltip();
}
// Wrapped help text in the disabled colour (ImGui::TextDisabled does not wrap, so long lines ran off the panel).
static void Note(const char* fmt, ...) {
    char b[1024]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", b);
    ImGui::PopStyleColor();
}
// Every collapsible section of the panel starts closed (eam::ui::SectionHeader with no second argument); the person opens what they need.
// A titled block of the panel: some room, a thin line, the title in the small capitals of the other apps, and a little room under it.
static void Block(const char* title, bool first = false) {
    const float u = ImGui::GetFontSize();
    if (!first) { ImGui::Dummy(ImVec2(0, u * 0.7f)); ImGui::Separator(); ImGui::Dummy(ImVec2(0, u * 0.5f)); }
    eam::ui::SectionLabel(title);
    ImGui::Dummy(ImVec2(0, u * 0.35f));
}

void DrawPanel() {
    FollowModelChoice();   // before the panel copies the settings: a model file chosen in the Runtimes list is not saved over
    Config c; { std::lock_guard<std::mutex> lk(g_settingsMutex); c = g_config; }
    bool changed = false, createChanged = false, tapChanged = false;
    // Every slider bound to a field of the params gets that field's default (a fresh NrParams): a tick on the groove, a ring on the
    // knob while it differs, double-click to reset. Sliders bound to anything else simply have no default.
    static const NrParams kDefaults;
    auto SL = [&](const char* label, float* v, float lo, float hi, const char* fmt = "%.2f") {
        const char* base = (const char*)&c.p; const char* pv = (const char*)v; const float* def = nullptr;
        if (pv >= base && pv < base + sizeof(NrParams)) def = (const float*)((const char*)&kDefaults + (pv - base));
        return eam::ui::SliderFloat(label, v, lo, hi, fmt, 0, def);
    };

    // status
    std::string status = Status(), frameInfo, adapterName; bool hasDisplay;
    { std::lock_guard<std::mutex> lk(g_textMutex); frameInfo = g_frameText.empty() ? g_frameText : g_frameText + " (" + g_encodingText + ")"; adapterName = g_cardName; hasDisplay = g_cardDrivesDisplay; }
    Block("Status", true);
    if (g_off) {
        std::string why; { std::lock_guard<std::mutex> lock(g_textMutex); why = g_offReason; }
        ImGui::PushStyleColor(ImGuiCol_Text, eam::ui::theme::V(eam::ui::theme::kDanger)); ImGui::Text("DISABLED: %s", why.c_str()); ImGui::PopStyleColor();
        ImGui::SameLine(); if (ImGui::SmallButton("Re-arm")) SwitchOn();
        Tip("Turn it back on after it switched itself off. If it switches off again, the reason above is still true.");
    }
    else { ImGui::PushStyleColor(ImGuiCol_Text, g_runs ? eam::ui::theme::V(eam::ui::theme::kAccent) : eam::ui::theme::V(eam::ui::theme::kWarn)); ImGui::Text("%s", status.c_str()); ImGui::PopStyleColor(); }
    if (g_engine.IsFailed()) { ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kDanger), "engine: %s", g_engine.Stats().lastError); ImGui::SameLine(); if (ImGui::SmallButton("Retry engine")) RestartEngine(); Tip("Try to start the DLSS model again on the current graphics card."); }
    if (!kScalerAddon) {   // ---- Requirements: what this needs, what was found, and what to do about anything missing
        const bool have = RequirementsScanned();
        const req::Report rep = Requirements({ g_engine.IsFailed(), g_engine.IsReady(), g_runs > 0, g_engine.Stats().lastError });
        Block("Requirements");
        ImGui::TextWrapped("You provide the model file yourself: nvngx_dlssnr.dll is not included with this addon and is never downloaded.");
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.25f));
        Note("1. Put your copy of nvngx_dlssnr.dll in the Lossless Scaling folder, next to LosslessScaling.exe. The Browse button below copies it there for you.");
        Note("2. Press Test compatibility to check that it works on your graphics card.");
        Note("3. Turn on Enable below, then start your game and scale it as usual.");
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.4f));
        // the verdict, in one line
        if (!have) ImGui::TextDisabled("Checking...");
        else if (rep.overall == req::Level::Missing) { ImGui::PushStyleColor(ImGuiCol_Text, eam::ui::theme::V(eam::ui::theme::kDanger)); ImGui::TextWrapped("Not ready yet. %s", rep.headline.c_str()); ImGui::PopStyleColor(); }
        else if (rep.overall == req::Level::Note) { ImGui::PushStyleColor(ImGuiCol_Text, eam::ui::theme::V(eam::ui::theme::kWarn)); ImGui::TextWrapped("Ready, with a note. %s", rep.headline.c_str()); ImGui::PopStyleColor(); }
        else { ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kAccent), "Everything is in place."); }
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));
        {   // (always open)
            for (const auto& row : rep.rows) {
                const ImVec4 col = row.level == req::Level::Ok ? eam::ui::theme::V(eam::ui::theme::kAccent) : (row.level == req::Level::Note ? eam::ui::theme::V(eam::ui::theme::kWarn) : eam::ui::theme::V(eam::ui::theme::kDanger));
                ImGui::TextColored(col, row.level == req::Level::Ok ? "OK     " : (row.level == req::Level::Note ? "NOTE   " : "MISSING"));
                ImGui::SameLine(ImGui::GetFontSize() * 5.2f); ImGui::Text("%s", row.label.c_str()); ImGui::SameLine(ImGui::GetFontSize() * 13.0f); ImGui::TextWrapped("%s", row.value.c_str());
                if (!row.hint.empty()) { ImGui::Indent(ImGui::GetFontSize() * 1.7f); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)); ImGui::TextWrapped("%s", row.hint.c_str()); ImGui::PopStyleColor(); ImGui::Unindent(ImGui::GetFontSize() * 1.7f); }
            }
            ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.4f));
            const bool placing = Browsing();
            if (placing) ImGui::BeginDisabled();
            if (ImGui::SmallButton(placing ? "Waiting for the file dialog..." : "Browse for the model file...")) BrowseForModel();
            if (placing) ImGui::EndDisabled();
            Tip("Pick your own copy of nvngx_dlssnr.dll: it is copied into the Lossless Scaling folder. A file already there is moved to the backups folder, never deleted. Nothing is downloaded.");
            ImGui::SameLine();
            const bool testing = SelfTesting();
            if (testing) ImGui::BeginDisabled();
            if (ImGui::SmallButton(testing ? "Testing..." : "Test compatibility")) RunSelfTest();
            if (testing) ImGui::EndDisabled();
            Tip("Runs the model file once, in a separate program, on your graphics card, and says whether it works there. It uses the graphics card for a few seconds, so do it before you start a game. It cannot take Lossless Scaling down if the model misbehaves.");
            ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.15f));
            if (ImGui::SmallButton("Open the Lossless Scaling folder")) ShellExecuteW(nullptr, L"open", g_lsDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            Tip("Where the model file, nvngx_dlssnr.dll, goes: next to LosslessScaling.exe.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Check again")) ScanRequirements();
            Tip("Look again at the graphics card, the NVIDIA driver, the model file and the helper DLL. Use it after putting a file in place.");
            const std::wstring reportFile = g_addonDir + L"\\compatibility_report.txt";
            if (GetFileAttributesW(reportFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Open the compatibility report")) ShellExecuteW(nullptr, L"open", reportFile.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                Tip("A short text file about the last compatibility test: your graphics card, driver, Windows, the model file's name, version and size, and the result. Paste it into an issue or the compatibility table. It holds no folders, user name or file hash.");
            }
            { bool ok; const std::string msg = BrowseResult(ok);
              if (!msg.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ok ? eam::ui::theme::V(eam::ui::theme::kAccent) : eam::ui::theme::V(eam::ui::theme::kWarn)); ImGui::TextWrapped("%s", msg.c_str()); ImGui::PopStyleColor(); } }
        }
    }
    else if (kFsrScaler) {   // ---- FSR's requirements: any DirectX 12 card, and AMD's runtime, which ships in the addon's fsr folder
        Block("Requirements");
        const std::wstring runtime = g_addonDir + L"\\fsr\\amd_fidelityfx_dx12.dll";
        const bool present = GetFileAttributesW(runtime.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (present) ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kAccent), "AMD's FSR runtime is in place (it comes with this addon).");
        else ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kDanger), "AMD's FSR runtime is missing: the addon's fsr folder should hold amd_fidelityfx_dx12.dll. Reinstall the addon.");
        Note("Works on any graphics card with DirectX 12 (AMD, NVIDIA or Intel). In Lossless Scaling choose NIS as the Scaling Type (FSR takes the place of that pass), "
             "and let the game run in a window smaller than your screen, for example 2560x1440 on a 4K screen; at the screen's own size it anti-aliases instead. "
             "Frame generation can be on or off. Only one of the FSR and DLSS Upscalers works at a time.");
    }
    else {   // ---- DLAA's requirements: an NVIDIA RTX card, and NVIDIA's runtime, which ships in the addon's dlss folder
        Block("Requirements");
        const std::wstring runtime = g_addonDir + L"\\dlss\\nvngx_dlss.dll";
        const bool present = GetFileAttributesW(runtime.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (present) ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kAccent), "NVIDIA's DLSS runtime is in place (it comes with this addon).");
        else ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kDanger), "NVIDIA's DLSS runtime is missing: the addon's dlss folder should hold nvngx_dlss.dll. Reinstall the addon.");
        Note("Needs an NVIDIA RTX graphics card. In Lossless Scaling choose NIS as the Scaling Type (DLSS takes the place of that pass), and let the game run in a window "
             "smaller than your screen so there is something to upscale, for example 2560x1440 on a 4K screen. Frame generation can be on or off: with it on, its motion is used.");
    }
    if (!kScalerAddon) {   // looks are Neural Rendering's
    Block("Saved looks (load and save your settings)");
    Note("A look is a saved set of the sliders below. Pick one from the list to load it. Save updates the look you picked; Save as new keeps the sliders as they are now under a name of your choice.");
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));
    // ---- Saved looks. Pick one to apply it; Save updates it (or asks for a name); Save as new keeps the current sliders under a
    // ---- new name; Delete asks first. A look is the sliders below (model knobs, picture, HUD areas), not the hotkeys or advanced settings.
    {
        static int s_active = -1; static char s_name[48] = ""; static bool s_askSave = false, s_askDelete = false;
        std::vector<std::string> names, datas;
        { std::lock_guard<std::mutex> lk(g_settingsMutex); for (auto& pr : g_looks) { names.push_back(pr.name); datas.push_back(pr.data); } }
        const std::string now = LookToText(c.p);
        if (s_active >= (int)names.size()) s_active = -1;
        // a look saved by an older version lacks newer keys; compare it as it would apply now, so it still reads as unchanged
        for (auto& d : datas) { NrParams t = c.p; ApplyLook(d, t); d = LookToText(t); }
        if (s_active < 0) for (size_t i = 0; i < datas.size(); ++i) if (datas[i] == now) { s_active = (int)i; break; }   // recognise the look already in use
        const bool modified = s_active >= 0 && datas[s_active] != now;
        const std::string label = s_active >= 0 ? names[s_active] + (modified ? "  (changed)" : "") : std::string("Custom");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 13.0f);
        if (ImGui::BeginCombo("Saved look", label.c_str())) {
            for (int i = 0; i < (int)names.size(); ++i)
                if (ImGui::Selectable(names[i].c_str(), i == s_active)) {
                    const float was = c.p.workingScale;
                    if (ApplyLook(datas[i], c.p)) { changed = true; if (c.p.workingScale != was) createChanged = true; }
                    s_active = i;
                }
            if (names.empty()) ImGui::TextDisabled("No saved looks yet: set the sliders, then Save as new.");
            ImGui::EndCombo();
        }
        Tip("Your saved looks: pick one to apply it. The Next preset hotkey cycles through them in the game. A look with a different Model resolution has the model made again in the background, which takes a fraction of a second.");
        ImGui::SameLine();
        if (eam::ui::Button("Save", eam::ui::icons::kSave, eam::ui::ButtonKind::Primary)) {
            if (s_active >= 0) {
                std::lock_guard<std::mutex> lk(g_settingsMutex);
                if (s_active < (int)g_looks.size()) g_looks[s_active].data = now;
                changed = true;
            } else { s_name[0] = 0; s_askSave = true; }
        }
        Tip(s_active >= 0 ? "Update the selected look with the sliders as they are now." : "Keep the sliders as they are now as a new look: you are asked for a name.");
        ImGui::SameLine();
        if (eam::ui::Button("Save as new", eam::ui::icons::kPlus)) { s_name[0] = 0; s_askSave = true; }
        Tip("Keep the current sliders under a new name. Using a name that already exists replaces that look.");
        ImGui::SameLine();   // always shown, so it can be found: greyed out until a look is picked
        if (s_active < 0) ImGui::BeginDisabled();
        if (eam::ui::Button("Delete", eam::ui::icons::kTrash, eam::ui::ButtonKind::Danger)) s_askDelete = true;
        if (s_active < 0) ImGui::EndDisabled();
        Tip(s_active >= 0 ? "Delete the selected look. Your current sliders are not changed." : "Pick a look in the list first, then this deletes it.");
        if (s_askSave) { ImGui::OpenPopup("Save look"); s_askSave = false; }
        if (ImGui::BeginPopupModal("Save look", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Name for this look");
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            const bool enter = ImGui::InputText("##lookname", s_name, sizeof s_name, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::Dummy(ImVec2(0, 4));
            const std::string n = CleanName(s_name);
            const bool can = !n.empty();
            if (!can) ImGui::BeginDisabled();
            if (eam::ui::Button("Save", eam::ui::icons::kCheck, eam::ui::ButtonKind::Primary) || (enter && can)) {
                { std::lock_guard<std::mutex> lk(g_settingsMutex); bool found = false; for (auto& pr : g_looks) if (pr.name == n) { pr.data = now; found = true; } if (!found) g_looks.push_back({ n, now }); }
                s_active = -1; changed = true; ImGui::CloseCurrentPopup();
            }
            if (!can) ImGui::EndDisabled();
            ImGui::SameLine();
            if (eam::ui::Button("Cancel", eam::ui::icons::kClose)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (s_askDelete) { ImGui::OpenPopup("Delete look"); s_askDelete = false; }
        if (ImGui::BeginPopupModal("Delete look", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete the look '%s'?", s_active >= 0 && s_active < (int)names.size() ? names[s_active].c_str() : "");
            ImGui::TextDisabled("Your current sliders stay as they are.");
            ImGui::Dummy(ImVec2(0, 4));
            if (eam::ui::Button("Delete", eam::ui::icons::kTrash, eam::ui::ButtonKind::Danger)) {
                { std::lock_guard<std::mutex> lk(g_settingsMutex); if (s_active >= 0 && s_active < (int)g_looks.size()) { ForgetLook(g_host, kAddonId, g_looks[s_active].name); g_looks.erase(g_looks.begin() + s_active); } }
                s_active = -1; changed = true; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (eam::ui::Button("Cancel", eam::ui::icons::kClose)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    }

    Block(kProductName);
    const bool dlaa = kScalerAddon;
    if (!kScalerAddon) {   // the upscalers are switched on and off in the addon list only (one switch, not two)
        const std::string label = std::string("Enable ") + kProductName;
        if (ImGui::Checkbox(label.c_str(), &c.enabled)) { changed = true; if (c.enabled) { ClaimFrames(); SwitchOn(); } else ReleaseFrames(); }
        Tip(kScalerAddon
                ? "Master switch. Off = Lossless Scaling's NIS runs as usual and the upscaler stops.\nTo compare while playing, use the Before / after hotkey instead: it keeps the upscaler running.\n"
                  "Only one of the DLSS and FSR Upscalers works at a time (switching one on in the addon list switches the other off); either works beside DLSS 5 Neural Rendering."
                : "Master switch. Off = Lossless Scaling runs untouched and the model stops.\nTo compare before and after while playing, use the Before / after hotkey instead: it keeps the model running.");
    }
    if (!kScalerAddon) ImGui::SameLine();
    if (ImGui::SmallButton("Reset history")) g_resetRequested = true;
    Tip("The model blends each frame with the ones before it. Press this after a scene cut, or if a ghost or smear seems stuck on screen.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Restore defaults")) {
        const float was = c.p.workingScale; const NrParams keep = c.p; c.p = ProductDefaults(); c.p.hudCount = keep.hudCount; memcpy(c.p.hud, keep.hud, sizeof c.p.hud); c.p.hudFeather = keep.hudFeather; c.lsFirst = true; changed = true; if (c.p.workingScale != was) createChanged = true;
    }
    Tip("Put the look and quality sliders back to this addon's defaults. Your presets, hotkeys and the advanced settings are not touched.");
    if (!kScalerAddon) {   // the other addon of the pair (the upscalers take the NIS pass and work beside Neural Rendering)
        const std::string owner = FrameOwner();
        if (!owner.empty() && owner != kAddonId)
            Note("%s is on now. Turning this on switches it off: only one of the two works on the frames at a time.", ProductNameOf(owner.c_str()));
    }
    bool modelChanged = false;
    if (kScalerAddon)
    {   // which FSR (or DLSS) runs: the same choice as the + beside it in the manager's Runtimes list
        static std::vector<RuntimeChoice> choices; static ULONGLONG listedAt = 0;
        if (!listedAt || GetTickCount64() - listedAt > 3000) { choices = RuntimeChoices(); listedAt = GetTickCount64(); }
        const std::wstring chosen = ChosenRuntimeFile();
        int current = 0;
        for (size_t i = 1; i < choices.size(); ++i) if (_wcsicmp(choices[i].path.c_str(), chosen.c_str()) == 0) current = (int)i;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        if (!choices.empty() && ImGui::BeginCombo(kFsrScaler ? "FSR version" : "DLSS version", choices[current].name.c_str())) {
            for (size_t i = 0; i < choices.size(); ++i)
                if (ImGui::Selectable(choices[i].name.c_str(), (int)i == current) && (int)i != current) ChooseRuntimeFile(choices[i].path);
            ImGui::EndCombo();
        }
        Tip(kFsrScaler ? "Which FSR runs. AMD's FSR 3.1.4 comes with the addon and runs on any card. FSR 4 is AMD's newer, machine-learning upscaler: "
                         "sharper and steadier in motion, and heavier; the OptiScaler team's build runs it on cards AMD's own does not (it is not signed). "
                         "Switching takes a second while the game runs. More files: the + next to FSR in the manager's Runtimes list (bottom left)."
                       : "Which DLSS runtime runs: the one that comes with the addon, or one added with the + next to DLSS in the manager's Runtimes list "
                         "(bottom left). Switching takes a moment while the game runs.");
    }
    if (dlaa && !kFsrScaler) {
        int preset = c.dlaaPreset == 13 ? 1 : c.dlaaPreset == 5 ? 2 : 0;
        const char* presets[] = { "NVIDIA's default (K, DLSS 4)", "M (DLSS 4.5, second-generation transformer)", "E (DLSS 3, the older CNN model)" };
        static const unsigned kPresetOf[] = { 0u, 13u, 5u };
        if (ImGui::Combo("DLSS model", &preset, presets, 3)) { c.dlaaPreset = kPresetOf[preset]; changed = true; modelChanged = true; }
        Tip("Which DLSS model runs. K is NVIDIA's default for DLAA. M is DLSS 4.5's newer model: sharper and steadier in motion in games, and much heavier "
            "(about three times K's time). E is DLSS 3's CNN model: lighter, and it keeps a still picture crisp where K and M can soften it here (users "
            "report it: Lossless Scaling's frames have no camera jitter, which those models expect). Compare them with the Before / after hotkey. "
            "Changing it restarts the engine.\n"
            "Lossless Scaling gives DLAA no camera jitter and no depth, so it smooths and steadies edges and shimmer but cannot add detail beyond the frame's own, as it does in a game that supports DLSS.");
    }

    Block("Settings");
    Note("Open a section to change it. Sliders: double-click to reset, Ctrl+click to type a value, Ctrl+scroll to fine-tune. The small tick marks the default.");
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));
    if (eam::ui::SectionHeader(dlaa ? "Motion" : "Model (what it does to the picture)")) {
        // Read by the model at every evaluate: changes apply on the next frame. Ranges are what the model honours
        // (docs/dlssnr-knobs.md): intensity clamps at 1, the local strengths do not clamp at all.
        if (!dlaa) {
        int style = (int)c.p.style; const char* styles[] = { "Standard", "Natural", "Cinematic" };
        if (ImGui::Combo("Style", &style, styles, 3)) { c.p.style = style; changed = true; }
        Tip("The model's three looks. Standard is the default. Natural compresses highlights by about 10% and can read as a dark vignette. Cinematic is the third variant.");
        changed |= SL("Model intensity", &c.p.intensity, 0.0f, 1.0f, "%.2f");
        Tip("How much of its edit the model produces. 0 = the model changes nothing; 1 = full. The model clamps at 1.\n(The Blend amount slider below is separate: it scales what gets added to the picture afterwards.)");
        changed |= SL("Fine detail strength", &c.p.localStructure, -2.0f, 5.0f, "%.2f");
        Tip("How strongly the model works on fine detail (edges, textures, text). 1 is the default. Above about 5 it turns to garbage; negative values give an 'anti-detail' look. Not clamped by the model.");
        changed |= SL("Local contrast", &c.p.localTone, -2.0f, 5.0f, "%.2f");
        Tip("How strongly the model works on local tone and contrast (how bright and dark areas separate). 1 is the default; same range behaviour as Fine detail strength.");
        changed |= SL("Skin and face detail", &c.p.skinStructure, -1.0f, 3.0f, c.p.skinStructure <= -0.99f ? "same as fine detail" : "%.2f");
        Tip("Detail strength where the model thinks it sees skin. At the far left it simply follows Fine detail strength. It only acts where skin is detected, so the change can be subtle.");
        bool am = c.p.useAutoMask != 0;
        if (ImGui::Checkbox("Detect skin automatically", &am)) { c.p.useAutoMask = am; changed = true; }
        Tip("Let the model find skin and faces on its own (the 'auto mask'). The effect is small; it is on by default.");
        }
        if (dlaa) {   // the upscaler: where DLSS's motion vectors come from
            const char* sources[] = { "Measured from the frames (any game)", "Lossless Scaling's frame generation", "None" };
            if (ImGui::Combo("Motion", &c.motionSource, sources, 3)) changed = true;
            Tip(kFsrScaler ? "FSR combines several frames, and needs to know where each pixel was in the frame before; a game with FSR built in tells it. Here:\n"
                           "Measured from the frames: the upscaler compares each frame with the one before and finds how every part of the picture moved. "
                           "Works with frame generation on or off, in any game. Costs a little GPU time (shown under Upscaling).\n"
                           "Lossless Scaling's frame generation: the motion its frame generation measures (only with frame generation on; coarser, a quarter of the game's size).\n"
                           "None: FSR assumes nothing moves. Sharp when still, smeared when the camera turns: this is here to compare." :
                "DLSS combines several frames, and needs to know where each pixel was in the frame before; a game with DLSS built in tells it. Here:\n"
                "Measured from the frames: the upscaler compares each frame with the one before and finds how every part of the picture moved. "
                "Works with frame generation on or off, in any game. Costs a little GPU time (shown under Upscaling).\n"
                "Lossless Scaling's frame generation: the motion its frame generation measures (only with frame generation on; coarser, a quarter of the game's size).\n"
                "None: DLSS assumes nothing moves. Sharp when still, smeared when the camera turns: this is here to compare.");
        }
        if (!dlaa) {
        changed |= ImGui::Checkbox("Use Lossless Scaling's motion data", &c.p.useFlow);
        Tip("Feeds the motion Lossless Scaling's frame generation measures (its optical flow) to the model as motion vectors, and uses it to slide the enhancement onto the generated in-between frames. Turn it off only to test without motion.");
        { const NrStats& fs = g_engine.Stats(); ImGui::SameLine(); if (fs.hasFlow) ImGui::TextDisabled("(flow %ux%u)", fs.flowW, fs.flowH); else ImGui::TextDisabled("(no flow texture seen yet)"); }
        {
            int mm = static_cast<int>(c.p.modelMotion); const char* mms[] = { "Measured from the frames (per pixel)", "Lossless Scaling's flow (coarse)" };
            if (!c.p.useFlow) ImGui::BeginDisabled();
            if (ImGui::Combo("The model's motion", &mm, mms, 2)) { c.p.modelMotion = static_cast<uint32_t>(mm); changed = true; }
            if (!c.p.useFlow) ImGui::EndDisabled();
            Tip("Where the model's motion vectors come from. Measured from the frames (the default): the addon compares each frame the model sees with the one "
                "before and finds how every part moved, per pixel and for this very frame, so the model's output holds together in motion with less smear. "
                "Lossless Scaling's flow: the motion its frame generation measures, a quarter of the frame's size. Either way the enhancement is slid onto the "
                "generated frames with Lossless Scaling's flow. The measurement costs a fraction of a millisecond at the model's working size.");
        }
        if (!c.p.useFlow) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Use this frame's motion (wait for Lossless Scaling's flow)", &c.freshFlow)) { changed = true; tapChanged = true; }
        Tip("On: the model runs a moment later in each frame, once Lossless Scaling has measured how this frame moved, so its motion vectors are current. "
            "Off: it runs as soon as the frame is captured and gets the previous frame's motion, one frame late, which smears and ghosts when the camera turns, starts or stops. "
            "On is the default; the switch is here to compare the two.");
        if (!c.p.useFlow) ImGui::EndDisabled();
        if (ImGui::Checkbox("Also with frame generation off", &c.presentMode)) changed = true;
        Tip("With Lossless Scaling's frame generation off there is no captured frame to run on. On (the default): the model then takes the frame Lossless Scaling "
            "presents. The model's working size is taken as for a 1920-wide frame, so it costs what it does with frame generation on. Off: without frame "
            "generation the addon does nothing.");
        if (!c.presentMode) ImGui::BeginDisabled();
        ImGui::Indent();
        if (ImGui::Checkbox("Wait for each frame's own result", &c.presentWait)) changed = true;
        Tip("Off (the default): each frame gets the newest result that is ready, usually the frame before's, moved along the measured motion to where the "
            "picture is now. Nothing waits, so the model's time is not added in front of each frame. On: each frame waits on the GPU for its own result. Exact, "
            "but the model's whole run (several milliseconds) comes before every frame is shown, which can make frames miss the display's refresh.");
        ImGui::Unindent();
        if (!c.presentMode) ImGui::EndDisabled();
        {
            static const char* const encodings[] = { "Automatic", "SDR", "HDR" };
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("Frame encoding", &c.frameEncoding, encodings, 3)) changed = true;
            Tip("What the frames hold, so an HDR picture is worked on as the model and the look controls expect. Automatic (the default): 8-bit frames are "
                "SDR, 16-bit float ones HDR (scRGB), and 10-bit ones HDR (HDR10) when Windows runs the display in HDR, SDR otherwise. HDR frames are "
                "brought to an SDR view with Windows' SDR content brightness as white, and only the change goes back into the frame, so highlights keep "
                "their brightness. SDR or HDR here overrides that for the 10-bit and 16-bit frames (8-bit ones are always SDR): set it only if the "
                "picture comes out washed out, too dark or too bright. The Technical status section shows what was decided.");
        }
        }
    }
    if (kScalerAddon && eam::ui::SectionHeader("Upscaling")) {
        const ScalerView v = GetScalerView();
        const char* const U = kUpscalerName;
        if (v.starting) ImGui::TextDisabled(kFsrScaler ? "Loading AMD's FSR runtime..." : "Loading NVIDIA's DLSS runtime...");
        else if (v.failed) ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kDanger), "%s could not run: %s. Lossless Scaling's NIS runs as usual.", U, v.error.c_str());
        else if (v.preparing) ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kWarn), "%s is getting ready for this frame format (the first frame can take a while); NIS runs meanwhile.", U);
        else if (!v.blocked.empty()) {   // the NIS pass is there but the upscaler cannot take it: say why, rather than look like it runs
            ImGui::PushStyleColor(ImGuiCol_Text, eam::ui::theme::V(eam::ui::theme::kWarn));
            if (v.nisSeen) ImGui::TextWrapped("NIS pass found (%ux%u -> %ux%u), but %s is not replacing it: %s", v.inW, v.inH, v.outW, v.outH, U, v.blocked.c_str());
            else ImGui::TextWrapped("%s is not replacing NIS: %s", U, v.blocked.c_str());
            ImGui::PopStyleColor();
        }
        else if (!v.nisSeen) ImGui::TextWrapped("Waiting for Lossless Scaling's NIS pass. Choose NIS as the Scaling Type and scale a game that runs in a window smaller than the screen.");
        else if (!v.ready) ImGui::TextDisabled("NIS pass found (%ux%u -> %ux%u); %s is not running yet.", v.inW, v.inH, v.outW, v.outH, U);
        else {
            const std::string running = v.provider.empty() ? std::string(U) : std::string(U) + " " + v.provider;   // "FSR 4.1.1b"
            ImGui::TextWrapped("%s upscales %ux%u -> %ux%u (x%.2f) in place of NIS: %.2f ms a frame on the GPU (motion %.2f ms of it), %llu frames so far%s.", running.c_str(), v.inW, v.inH,
                               v.outW, v.outH, v.inW ? (float)v.outW / v.inW : 0.0f, v.gpuMs, v.motionMs, (unsigned long long)v.runs,
                               v.perFrame > 1 ? " (real and generated frames alike)" : "");
            if (v.second.valid) {
                ImGui::TextDisabled("Last second: %.0f pictures a second (%.0f%% in close pairs); %.1f%% shown twice, %.1f%% of frames not handed over, %.1f%% waited for on the GPU.",
                                    v.second.fps, v.second.closePct, v.second.repeatPct, v.second.skipPct, v.second.waitPct);
                Tip("Close pairs: two frames within 6 ms of each other, as adaptive frame generation makes them. Shown twice: the upscaler had not finished a newer picture "
                    "when Lossless Scaling needed one, so the one before was shown again (a small judder). Waited for on the GPU: rather than that, Lossless Scaling's "
                    "frame waited on the GPU for the next picture (the setting below). Not handed over: the upscaler still had two frames to do, so this one was "
                    "left out. A GPU running at its limit makes these rise: a frame cap with some headroom helps most.");
            }
            if (g_compare.load() == 2) ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kWarn), "Showing Lossless Scaling's NIS for comparison (Before / after hotkey).");
        }
        changed |= SL("Sharpening", &c.p.sharpen, 0.0f, 1.0f, c.p.sharpen <= 0.001f ? "off" : "%.2f");
        if (kFsrScaler) Tip("FSR's own sharpening (AMD's RCAS), part of its upscaling pass; above about 0.6 an extra pass adds more than RCAS can. Lossless Scaling's "
                            "NIS sharpens too (its Sharpness setting), so without it FSR can look softer next to NIS. 0.5 is a good start (Ctrl+Shift+F8 / F9 in the game).");
        else Tip("Contrast-adaptive sharpening of DLSS's picture (the FidelityFX CAS formula), which costs a fraction of a millisecond; above about 0.6 its effect is amplified "
                 "past CAS's own maximum. DLSS has no sharpening of its own, while Lossless Scaling's NIS does (its Sharpness setting), so without it DLSS can look softer "
                 "next to NIS. 0.5 is a good start (Ctrl+Shift+F8 / F9 in the game).");
        // FSR 4 keeps its history its own way and takes none of FSR 3.1's tuning (nor the mask the slider widens): the slider would do nothing
        const bool fsr4 = kFsrScaler && GetScalerView().provider.rfind("4", 0) == 0;
        if (fsr4) ImGui::BeginDisabled();
        { const float off = 0.0f; changed |= eam::ui::SliderFloat("Stability", &c.scalerStability, 0.0f, 1.0f, c.scalerStability <= 0.001f ? "off" : "%.2f", 0, &off); }
        if (fsr4) { ImGui::EndDisabled(); Note("FSR 4 handles stability by itself: this slider is for FSR 3.1."); }
        { char tip[768];
          snprintf(tip, sizeof tip, "Less shimmer on thin lines, wires and leaves, at the cost of more trailing behind what moves. The game gives the upscaler no "
                   "sub-pixel camera shifts, so a line thinner than a pixel flickers from frame to frame; with this up, the motion measurement stops reporting that "
                   "flicker as a place not to trust the history, so %s averages it out over the frames before%s. Off: as before. Try 0.5, then move it while "
                   "you look at a fence or a power line. The game's own anti-aliasing (MSAA) is still what draws thin lines in the first place.",
                   U, kFsrScaler ? ", and FSR keeps more of its history and reacts less to small changes of shading" : "");
          Tip(tip); }
        { const float off = 0.0f; changed |= eam::ui::SliderFloat("Edge smoothing", &c.scalerEdges, 0.0f, 1.0f, c.scalerEdges <= 0.001f ? "off" : "%.2f", 0, &off); }
        Tip("Anti-aliasing along the edges of the upscaled picture: for games without anti-aliasing of their own (stair steps on roofs, fences and "
            "wires). It finds where the brightness steps, which way the edge runs and how far, and blends across it by the part of a pixel the true "
            "edge would cover. Costs a fraction of a millisecond. It also softens text a little. If the game has anti-aliasing (MSAA), that is "
            "better: use this where it has none.");
        {   // the picture controls: on the frame before it is upscaled; Neural Rendering's own take over while it is on
            const bool nrOn = NeuralRenderingOn();
            if (nrOn) ImGui::BeginDisabled();
            { const float d = 0.0f; changed |= eam::ui::SliderFloat("Vibrance", &c.p.vibrance, 0.0f, 1.0f, c.p.vibrance <= 0.001f ? "off" : "%.2f", 0, &d); }
            Tip("Lifts muted colours much more than vivid ones, so skin and already-strong colours are not pushed further: a natural colour boost.");
            { const float d = 1.0f; changed |= eam::ui::SliderFloat("Saturation", &c.p.saturation, 0.0f, 2.0f, std::abs(c.p.saturation - 1.0f) < 0.005f ? "unchanged" : "%.2f", 0, &d); }
            Tip("More colour everywhere (above 1) or less (below 1, 0 is black and white).");
            { const float d = 0.0f; changed |= eam::ui::SliderFloat("Shadows", &c.p.shadows, -1.0f, 1.0f, std::abs(c.p.shadows) < 0.005f ? "unchanged" : "%+.2f", 0, &d); }
            Tip("Lifts (right) or deepens (left) the dark parts only: see into a dark interior or a night without greying the rest.");
            { const float d = 0.0f; changed |= eam::ui::SliderFloat("Highlights", &c.p.highlights, -1.0f, 1.0f, std::abs(c.p.highlights) < 0.005f ? "unchanged" : "%+.2f", 0, &d); }
            Tip("Tames (left) or brightens (right) the bright parts only: a blown-out sky or snow gets detail back.");
            { const float d = 0.0f; changed |= eam::ui::SliderFloat("Brightness", &c.p.brightness, -0.3f, 0.3f, "%+.2f", 0, &d); }
            Tip("Lifts or lowers the whole picture, as a monitor's brightness does. For a game that is too dark (or too bright) to see into.");
            { const float d = 1.0f; changed |= eam::ui::SliderFloat("Contrast", &c.p.contrast, 0.5f, 1.5f, "%.2f", 0, &d); }
            Tip("Spreads the tones apart (above 1) or draws them together (below 1), around mid-grey.");
            { const float d = 1.0f; changed |= eam::ui::SliderFloat("Gamma", &c.p.gamma, 0.5f, 2.0f, "%.2f", 0, &d); }
            Tip("Above 1 brightens the mid-tones and leaves black and white where they are: dark corners get visible without washing out the "
                "sky. Below 1 darkens them.");
            if (nrOn) { ImGui::EndDisabled(); Note("DLSS 5 Neural Rendering is on: its Picture section sets these now."); }
        }
        {
            std::string game; { std::lock_guard<std::mutex> lk(g_settingsMutex); game = g_scalerGame; }
            if (ImGui::Checkbox("Keep these settings per game", &c.scalerPerGame)) changed = true;
            Tip("On (the default): sharpening, stability, edge smoothing, the colour and tone controls, the DLSS model and the motion are kept for each game, and come back when "
                "that game takes focus. A change made here counts for the game played last.");
            if (c.scalerPerGame) {
                ImGui::SameLine();
                ImGui::TextDisabled(game.empty() ? "(no game seen yet)" : "for %s", game.c_str());
                if (!c.scalerGames.empty()) {
                    int forget = -1;
                    if (ImGui::TreeNode("Games with settings of their own")) {
                        for (size_t i = 0; i < c.scalerGames.size(); ++i) {
                            const auto& [exe, p] = c.scalerGames[i];
                            ImGui::PushID(static_cast<int>(i));
                            ImGui::AlignTextToFramePadding();
                            ImGui::Text("%s: sharpening %.2f, stability %.2f, edges %.2f", exe.c_str(), p.sharpen, p.stability, p.edges);
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Forget")) forget = static_cast<int>(i);
                            ImGui::PopID();
                        }
                        ImGui::TreePop();
                    }
                    if (forget >= 0) { c.scalerGames.erase(c.scalerGames.begin() + forget); changed = true; }
                }
            }
        }
        if (ImGui::Checkbox("Wait on the GPU rather than repeat a picture", &c.scalerGpuWait)) changed = true;
        Tip("When the upscaler has not finished a new picture by the time Lossless Scaling needs one (two frames close together, as adaptive frame generation makes "
            "them), the picture before would be shown again: a small judder. On (the default), Lossless Scaling's frame waits on the GPU for the new picture instead, "
            "as a game with DLSS built in waits for DLSS; the CPU never waits. Off: the picture before is shown again. Compare the line above with it on and off.");
        Note("Compare with the Before / after hotkey (Compare and hotkeys): it switches between %s and Lossless Scaling's own NIS while you play.", U);
        {   // HDR games: the frames are upscaled in their SDR view and put back in their own encoding (scaler11.h, SetEncoding)
            static const char* const encodings[] = { "Automatic", "SDR", "HDR" };
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("Frame encoding", &c.frameEncoding, encodings, 3)) changed = true;
            Tip("For HDR games. Automatic (the default) works it out: 8-bit frames are SDR, 16-bit float ones HDR (scRGB), and 10-bit ones HDR (HDR10) "
                "when Windows runs the display in HDR. Set SDR or HDR only if the picture comes out washed out, too dark or too bright. "
                "Technical status shows what was decided.");
        }
    }
    if (!kScalerAddon && eam::ui::SectionHeader("Quality and performance")) {
        // The model costs ~10 ms + ~7 ms per megapixel on Ampere. The working scale is the only cost lever: past the
        // frame interval the model simply skips frames and the present side carries the last delta forward.
        if (dlaa) Note("DLAA works on the whole frame, so there is no model resolution to choose.");
        else {
        createChanged |= SL("Model resolution", &c.p.workingScale, 0.25f, 1.0f, "%.2f x the frame");
        Tip("The frame is shrunk by this before the model sees it, and the model's change is stretched back up to the picture. 1.00 = the model sees the whole frame: best detail, costs the most. This is the only setting that changes the cost.\nOn an RTX 4070 Ti SUPER the model takes roughly 2.7 ms plus 1.8 ms per megapixel.\nChanging it has the model made again in the background: for a fraction of a second the last result carries on, and the game does not stall.\nWith Auto on, this is the most it uses.");
        changed |= ImGui::Checkbox("Auto: keep the model within a time budget", &c.autoQuality);
        Tip("Lowers the model resolution while the model takes longer than the budget below (a busy scene, a hot card), and raises it back toward your own setting above when there is room. It changes slowly (down after 3 s over the budget, up after 10 s well under it) so it judges steady numbers, not a moment's spike. Nothing that changes the look is touched.");
        if (c.autoQuality) {
            changed |= SL("Time budget", &c.autoBudgetMs, 2.0f, 15.0f, "%.1f ms");
            Tip("The model time to stay within. Half the frame time is a good start: 8 ms at 60 fps, 5 ms at 100 fps.");
            changed |= SL("Lowest model resolution", &c.autoFloor, 0.25f, 1.0f, "%.2f x the frame");
            Tip("Auto never goes below this, even when the model still runs over the budget.");
            float scale, avg; std::deque<AutoQuality::Step> history;
            { std::lock_guard<std::mutex> lock(g_autoMutex); scale = g_auto.Scale(); avg = g_auto.AverageMs(); history = g_auto.History(); }
            if (scale <= 0) ImGui::TextDisabled("Auto starts with the model's first run.");
            else if (scale < c.p.workingScale - 0.001f) ImGui::TextWrapped("Auto runs the model at %.2f x (model %.1f ms, budget %.1f ms).", scale, avg, c.autoBudgetMs);
            else ImGui::TextWrapped("Auto runs the model at your setting, %.2f x (model %.1f ms, budget %.1f ms).", scale, avg, c.autoBudgetMs);
            if (!history.empty() && ImGui::TreeNode("autohist", "Recent changes (%d)", static_cast<int>(history.size()))) {
                const uint64_t now = GetTickCount64();
                for (auto it = history.rbegin(); it != history.rend(); ++it)
                    ImGui::Text("%3llu s ago: %.2f -> %.2f (model %.1f ms)", static_cast<unsigned long long>((now - it->atMs) / 1000), it->from, it->to, it->modelMs);
                ImGui::TreePop();
            }
        }
        }
        {
            const NrStats& st = g_engine.Stats();
            if (g_bridge.Width()) {
                float mp = (float)st.workW * (float)st.workH / 1e6f; double iv = g_bridge.IntervalMs();
                uint64_t runs = g_bridge.Runs(), skipped = g_bridge.Skipped(), seen = runs + skipped;
                ImGui::TextWrapped("frame %ux%u -> model input %ux%u (%.2f MP): model %.1f ms (avg %.1f), frame interval %.1f ms", g_bridge.Width(), g_bridge.Height(), st.workW, st.workH, mp, st.nrMs, g_avgModelMs, iv);
                if (st.builds) ImGui::TextWrapped("model made %u time%s, the last in %.0f ms off the frame path; %llu frames not run meanwhile", st.builds, st.builds == 1 ? "" : "s", st.lastBuildMs, (unsigned long long)st.busySkips);
                ImGui::TextWrapped("model keeps up with %llu of %llu frames (%.0f%%); GPU start +%.1f / done +%.1f ms after submit; tap CPU %.2f ms", (unsigned long long)runs, (unsigned long long)seen, seen ? 100.0 * runs / seen : 0.0, st.startMs, st.doneMs, g_bridge.CpuMs());
                ImGui::TextWrapped("presents: %llu on LS's swap chain (%s), composed %llu, compose CPU %.2f ms, target %s; newest delta = frame %llu, applied at offset %.2f frames",
                    (unsigned long long)g_lsPresents, g_tap.PresentPattern(), (unsigned long long)g_composed, g_compose.CpuMs(), g_compose.TargetInfo(), (unsigned long long)g_lastDelta, g_lastOffset);
                if (seen > 30 && runs * 2 < seen) ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kWarn), "the model runs on fewer than half of the frames: the delta is carried across frames by the flow. Lower the working scale for a fresher result.");
            } else ImGui::TextDisabled("(no frame tapped yet)");
        }
        if (!dlaa) {
        { int ps = (int)c.p.passes; const int dps = (int)kDefaults.passes; if (eam::ui::SliderInt("Model passes", &ps, 1, 4, "%d", 0, &dps)) { c.p.passes = (uint32_t)ps; changed = true; } }
        Tip("How many times the model reworks each frame; every pass takes the previous result as its input. 1 = normal. 2 to 4 make the effect stronger (and can start to look over-processed, so compare with the Before / after hotkey).\nEach extra pass costs roughly another model run: watch the model time and the 'keeps up with' line below. If the model cannot keep up it skips frames, and the last result is carried forward.");
        }
        changed |= SL("Temporal smoothing", &c.p.deltaSmooth, 0.0f, 0.9f, c.p.deltaSmooth <= 0.001f ? "off" : "%.2f");
        Tip("Blends the model's change for this frame with its change for the previous one, moved along with the picture by Lossless Scaling's motion data. It calms shimmer and crawling in fine detail (distant roads, fences, foliage) at the price of a little softness or ghosting when the camera moves fast. 0 = off; try 0.3 first. Costs almost nothing.");
        changed |= ImGui::Checkbox("Give Lossless Scaling GPU priority", &c.lsFirst);
        Tip("Raises Lossless Scaling's own graphics work above the model's on the shared card, so frame generation and presenting are not delayed while the model runs. Recommended.");
        changed |= SL("Blend amount", &c.p.composeIntensity, 0.0f, 2.0f);
        Tip("How much of the model's change is added to each presented frame. 1 = exactly what the model made; 0 = none; above 1 exaggerates it.");
        changed |= SL("Ghost guard", &c.p.ghostGuard, 0.0f, 1.0f, c.p.ghostGuard <= 0.001f ? "off" : "%.2f");
        Tip("Stops the faint copy of the previous frame that can trail moving things. The model works on an older frame and its change is moved onto the current one with Lossless Scaling's motion data; where that motion data is unreliable (the edge of a moving object, something just uncovered) the change lands in the wrong place. This fades the change out there, and a little more the older it is, and leaves still and steadily moving areas alone. 0 = off, 0.5 = a good start, 1 = strongest. To see where it acts, set Diagnostic view to Ghost guard: dark areas are faded.");
        changed |= SL("Limit per-pixel change", &c.p.maxDelta, 0.05f, 1.0f);
        Tip("The most any pixel's colour may be changed (0..1 of full range). Lower is safer and subtler; it stops the model from making harsh jumps.");
        changed |= SL("Protect bright areas from", &c.p.hiProtect, 0.5f, 1.0f, c.p.hiProtect >= 0.999f ? "off" : "%.2f");
        Tip("The model's change fades out as a pixel's brightness rises from this level to white, so highlights are not crushed. At the far right (off) the change applies everywhere.");
    }
    if (!kScalerAddon && eam::ui::SectionHeader("Picture (sharpness, tone, colour, grain)")) {
        // Compose side, applied to every presented frame, real and generated alike.
        changed |= SL("Sharpen", &c.p.sharpen, 0.0f, 1.0f, c.p.sharpen <= 0.001f ? "off" : "%.2f");
        Tip("Contrast-adaptive sharpening of every presented frame, after the model's change is added. The model and the upscale both soften the picture; a little sharpening (0.2 to 0.4) puts the bite back. Costs almost nothing.");
        changed |= SL("Saturation", &c.p.saturation, 0.0f, 2.0f, fabsf(c.p.saturation - 1.0f) < 0.005f ? "unchanged" : "%.2f");
        Tip("Colour intensity of the finished picture. 1.00 = unchanged; 0 = black and white; above 1 = more vivid. Applied last, to real and generated frames alike. Costs nothing measurable.");
        changed |= SL("Vibrance", &c.p.vibrance, 0.0f, 1.0f, c.p.vibrance <= 0.001f ? "off" : "%.2f");
        Tip("Like Saturation, but it lifts muted colours much more than vivid ones, so skin tones and already-strong colours are not pushed further. Use this for a gentle, natural colour boost; use Saturation for a blunt one.");
        changed |= SL("Brightness", &c.p.brightness, -0.3f, 0.3f, fabsf(c.p.brightness) < 0.0005f ? "unchanged" : "%+.2f");
        Tip("Lifts or lowers every pixel by the same amount (on the picture's own 0 to 1 scale). Simple, but it also lifts blacks, so it can wash the picture out: for a brighter look without that, try Gamma first.");
        changed |= SL("Contrast", &c.p.contrast, 0.5f, 1.5f, fabsf(c.p.contrast - 1.0f) < 0.002f ? "unchanged" : "%.2f");
        Tip("Pushes the picture away from mid-grey (above 1) or toward it (below 1). Blacks get darker and whites brighter as it rises; some detail in the extremes can clip.");
        changed |= SL("Gamma", &c.p.gamma, 0.5f, 2.0f, fabsf(c.p.gamma - 1.0f) < 0.002f ? "unchanged" : "%.2f");
        Tip("Bends the mid-tones without touching pure black or pure white. Above 1 brightens the mid-tones (opens up dark scenes); below 1 darkens them. Usually the best brightness control.");
        changed |= SL("Shadows", &c.p.shadows, -1.0f, 1.0f, fabsf(c.p.shadows) < 0.005f ? "unchanged" : "%+.2f");
        Tip("Works on the dark parts of the picture only. Above 0 lifts them (opens up dark corners and dungeons); below 0 deepens them. Bright areas stay as they are.");
        changed |= SL("Highlights", &c.p.highlights, -1.0f, 1.0f, fabsf(c.p.highlights) < 0.005f ? "unchanged" : "%+.2f");
        Tip("Works on the bright parts of the picture only. Below 0 pulls them down (recovers sky and glare); above 0 pushes them up. Dark areas stay as they are.");
        changed |= SL("Film grain", &c.p.grain, 0.0f, 1.0f, c.p.grain <= 0.002f ? "off" : "%.2f");
        Tip("Fine monochrome noise, strongest in the mid-tones and different on every frame. Hides banding and the plastic look of upscaling. Keep it low (0.1 to 0.3).");
        { int gs = (int)c.p.grainSize; const int dgs = (int)kDefaults.grainSize; if (eam::ui::SliderInt("Grain size", &gs, 1, 4, "%d px", 0, &dgs)) { c.p.grainSize = (float)gs; changed = true; } }
        Tip("How big each grain speck is, in screen pixels. 1 is the finest; on a 4K screen 2 looks closest to film.");
    }
    if (!kScalerAddon && eam::ui::SectionHeader("Keep the HUD untouched")) {
        Note("Areas where the picture stays exactly as Lossless Scaling made it (no model change, sharpening, tone, colour or grain): for action bars, the minimap, chat and text. Draw them on a snapshot of the game.");
        const bool busy = screenshot::Busy();
        if (busy) ImGui::BeginDisabled();
        if (eam::ui::Button(busy ? "Taking it..." : "Take a snapshot", eam::ui::icons::kCheck, eam::ui::ButtonKind::Primary)) screenshot::RequestSnapshot();
        if (busy) ImGui::EndDisabled();
        Tip("A picture of the game as it is shown now, to draw the areas on. The game must be running and scaled. Take another one any time.");
        ImGui::SameLine();
        { bool show = g_showHud; if (ImGui::Checkbox("Show the areas in the game", &show)) g_showHud = show; }
        Tip("Tints and outlines the areas in green in the game itself, to check them against your HUD. Not saved: turn it off when you are done.");
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));
        changed |= DrawHudEditor(c.p);
        Note("Drag on the picture to add an area (six at most). Drag an area to move it, an edge or corner to resize it. Right-click an area to remove it.");
        changed |= SL("Edge softness", &c.p.hudFeather, 0.0f, 0.05f, c.p.hudFeather <= 0.0005f ? "hard edge" : "%.3f");
        Tip("How gradually the enhancement fades in outside an area, as a fraction of the screen. 0 = a hard edge.");
        if (ImGui::SmallButton("WoW starter layout")) {
            const float d[4][4] = { { 0.0f, 0.0f, 0.30f, 0.17f }, { 0.86f, 0.0f, 1.0f, 0.24f }, { 0.0f, 0.70f, 0.26f, 1.0f }, { 0.27f, 0.88f, 0.73f, 1.0f } };
            memcpy(c.p.hud, d, sizeof d); c.p.hudCount = 4; changed = true;
        }
        Tip("Four areas where World of Warcraft's default interface sits: unit frames (top left), minimap (top right), chat (bottom left) and the action bars (bottom centre). A starting point to adjust on the snapshot.");
        if (c.p.hudCount) { ImGui::SameLine(); if (ImGui::SmallButton("Clear all")) { c.p.hudCount = 0; changed = true; } }
        if (c.p.hudCount && ImGui::TreeNode("Exact numbers")) {
            for (uint32_t i = 0; i < c.p.hudCount; ++i) {
                ImGui::PushID(static_cast<int>(i));
                float* r = c.p.hud[i];
                ImGui::Text("Area %u", i + 1);
                changed |= eam::ui::SliderFloat("Left", &r[0], 0.0f, 0.99f); changed |= eam::ui::SliderFloat("Top", &r[1], 0.0f, 0.99f);
                changed |= eam::ui::SliderFloat("Right", &r[2], 0.01f, 1.0f); changed |= eam::ui::SliderFloat("Bottom", &r[3], 0.01f, 1.0f);
                r[2] = std::max(r[2], r[0] + 0.01f); r[3] = std::max(r[3], r[1] + 0.01f);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        Note("Areas are saved with the look, so each game can have its own layout.");
    }
    if (kScalerAddon && eam::ui::SectionHeader("Compare and hotkeys")) {   // the upscalers: the upscaled picture or NIS's, and the sharpening
        int cm = g_compare.load() == 2 ? 1 : 0; const char* cms[] = { "Upscaled", "Lossless Scaling's NIS (before)" };
        if (ImGui::Combo("Compare view", &cm, cms, 2)) g_compare = cm == 1 ? 2 : 0;
        Tip("Upscaled = normal. Lossless Scaling's NIS = as if the addon were off, to compare (the upscaler stays loaded, so switching back is instant). Not saved.");
        changed |= ImGui::Checkbox("Hotkeys: Ctrl+Shift + key (work while the game has focus and the upscaler runs)", &c.hotkeys);
        Tip("Switch between the upscaler and NIS, and change the sharpening, from inside the game. The Ctrl+Shift pair keeps them away from the game's own key bindings.");
        auto fkey = [&](const char* label, int* vk) {
            int idx = *vk - VK_F1; if (idx < 0 || idx > 11) idx = 5;
            const char* names[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
            const ImGuiStyle& st = ImGui::GetStyle();
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("F12").x + st.FramePadding.x * 2.0f + ImGui::GetFrameHeight() + st.ItemInnerSpacing.x);
            if (ImGui::Combo(label, &idx, names, 12)) { *vk = VK_F1 + idx; changed = true; }
        };
        fkey("Before / after", &c.keyAB); fkey("Sharpen -", &c.keySharpDn); fkey("Sharpen +", &c.keySharpUp); fkey("Save the recording", &c.keyRecord);
        fkey("Before / after pictures", &c.keyShot);
        auto fname = [](int vk) { static char b[4][8]; static int n = 0; char* o = b[n++ & 3]; snprintf(o, 8, "F%d", vk - VK_F1 + 1); return (const char*)o; };
        Note("Now: Ctrl+Shift+%s before/after  |  %s / %s sharpen - / +  |  %s save the recording  |  %s before/after pictures  (%s)", fname(c.keyAB),
             fname(c.keySharpDn), fname(c.keySharpUp), fname(c.keyRecord), fname(c.keyShot), c.hotkeys ? "hotkeys on" : "hotkeys OFF: tick the box above");
        if (ImGui::SmallButton("Save before / after pictures")) RequestPair();
        Tip("Two PNGs of the same moment: the upscaled picture and Lossless Scaling's NIS (one frame of NIS shows on screen while it is taken). "
            "Windows' own screenshots show the game's window, not Lossless Scaling's picture. HDR games are saved in their SDR view.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Open the pictures folder")) ShellExecuteW(nullptr, L"open", screenshot::Folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        { bool ok; const std::string msg = screenshot::LastResult(ok);
          if (!msg.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ok ? eam::ui::theme::V(eam::ui::theme::kAccent) : eam::ui::theme::V(eam::ui::theme::kWarn)); ImGui::TextWrapped("%s", msg.c_str()); ImGui::PopStyleColor(); } }
    }
    if (!kScalerAddon && eam::ui::SectionHeader("Compare and hotkeys")) {
        int cm = g_compare; const char* cms[] = { "Enhanced", "Split: left original | right enhanced", "Original only (before)" };
        if (ImGui::Combo("Compare view", &cm, cms, 3)) g_compare = cm;
        Tip("Enhanced = normal. Split = left of the line is the original, right is enhanced. Original only = as if the addon were off (it saves the compose work but the model keeps running). Display only; not saved.");
        float sp = g_splitPos; { const float dsp = 0.5f; if (eam::ui::SliderFloat("Split position", &sp, 0.05f, 0.95f, "%.2f", 0, &dsp)) g_splitPos = sp; }
        Tip("Where the split line sits, from the left edge (0) to the right edge (1) of the screen.");
        Note("Display only: the model keeps running, so switching is instant. Not saved: Lossless Scaling always starts enhanced.");
        changed |= ImGui::Checkbox("Hotkeys: Ctrl+Shift + key (work while the game has focus)", &c.hotkeys);
        Tip("Switch the compare view, sharpen and presets from inside the game. They are read while Lossless Scaling is presenting frames. The Ctrl+Shift pair keeps them away from the game's own key bindings.");
        auto fkey = [&](const char* label, int* vk) {
            int idx = *vk - VK_F1; if (idx < 0 || idx > 11) idx = 5;
            const char* names[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
            // wide enough for "F12" plus the arrow at any display scale (a fixed pixel width clipped the key name)
            const ImGuiStyle& st = ImGui::GetStyle();
            ImGui::SetNextItemWidth(ImGui::CalcTextSize("F12").x + st.FramePadding.x * 2.0f + ImGui::GetFrameHeight() + st.ItemInnerSpacing.x);
            if (ImGui::Combo(label, &idx, names, 12)) { *vk = VK_F1 + idx; changed = true; }
        };
        fkey("Before / after", &c.keyAB); fkey("Split view", &c.keySplit); fkey("Sharpen -", &c.keySharpDn); fkey("Sharpen +", &c.keySharpUp); fkey("Next preset", &c.keyPreset);
        fkey("Screenshot", &c.keyShot); fkey("Save the recording", &c.keyRecord);
        auto fname = [](int vk) { static char b[8][8]; static int n = 0; char* o = b[n++ & 7]; snprintf(o, 8, "F%d", vk - VK_F1 + 1); return (const char*)o; };
        Note("Now: Ctrl+Shift+%s before/after  |  %s split  |  %s / %s sharpen - / +  |  %s next preset  |  %s screenshot  |  %s save the recording  (%s)",
            fname(c.keyAB), fname(c.keySplit), fname(c.keySharpDn), fname(c.keySharpUp), fname(c.keyPreset), fname(c.keyShot), fname(c.keyRecord),
            c.hotkeys ? "hotkeys on" : "hotkeys OFF: tick the box above");
        Note("A small square appears in the screen's top-left corner for a moment: green enhanced, red original, amber split, blue sharpen changed, purple preset.");
    }
    if (!kScalerAddon && eam::ui::SectionHeader("Screenshots")) {
        Note("Saves the picture as you see it, with the addon's result, the scaling and frame generation in it, as a PNG. It is taken at the next frame Lossless Scaling shows, so the game must be running and scaled.");
        const bool busy = screenshot::Busy();
        if (busy) ImGui::BeginDisabled();
        if (eam::ui::Button(busy ? "Taking it..." : "Take a screenshot", eam::ui::icons::kCheck, eam::ui::ButtonKind::Primary)) screenshot::Request();
        if (busy) ImGui::EndDisabled();
        Tip("In the game, Ctrl+Shift + the Screenshot key (see Compare and hotkeys) does the same.");
        const std::wstring folder = screenshot::Folder();
        const int size = WideCharToMultiByte(CP_UTF8, 0, folder.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string folderText(size > 0 ? size - 1 : 0, '\0');
        if (size > 1) WideCharToMultiByte(CP_UTF8, 0, folder.c_str(), -1, folderText.data(), size, nullptr, nullptr);
        ImGui::TextWrapped("Folder: %s", folderText.c_str());
        const bool choosing = screenshot::Choosing();
        if (choosing) ImGui::BeginDisabled();
        if (ImGui::SmallButton(choosing ? "Waiting for the folder dialog..." : "Choose folder...")) screenshot::ChooseFolder();
        if (choosing) ImGui::EndDisabled();
        Tip("Where the screenshots go. The first time, a folder called Lossless Scaling in your Pictures folder.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Open folder")) { CreateDirectoryW(folder.c_str(), nullptr); ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
        if (!c.screenshotFolder.empty()) { ImGui::SameLine(); if (ImGui::SmallButton("Use Pictures again")) { c.screenshotFolder.clear(); changed = true; } }
        bool ok; const std::string result = screenshot::LastResult(ok);
        if (!result.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ok ? eam::ui::theme::V(eam::ui::theme::kAccent) : eam::ui::theme::V(eam::ui::theme::kWarn)); ImGui::TextWrapped("%s", result.c_str()); ImGui::PopStyleColor(); }
    }
    if (eam::ui::SectionHeader("Recording (for bug reports and tests)")) {
        Note(kScalerAddon ? "Keeps the last few seconds of the frames the upscaler receives (Lossless Scaling's frame as it goes to the upscaler, with the colour and tone settings), losslessly, ready to "
                            "save as a .lsrec file. Send one with a bug report and the problem can be replayed and fixed on another computer."
                          : "Keeps the last few seconds of the frames Neural Rendering receives (the game's frames before the model or anything else changes "
                            "them), losslessly, ready to save as a .lsrec file. Send one with a bug report and the problem can be replayed and fixed on "
                            "another computer.");
        if (ImGui::Checkbox("Keep the last few seconds ready to save", &c.recordOn)) {
            changed = true;
            g_recorder.Configure(c.recordOn, c.recordSeconds, static_cast<uint32_t>(c.recordBudgetMb));   // off: its memory goes now, not at the next frame
        }
        Tip("Off (the default) costs nothing. On: every frame is copied off the graphics card and compressed on a few background threads, which "
            "takes some processor time and the memory below. Nothing waits for it: a frame that comes while all the copies are busy is left out.");
        if (!c.recordOn) ImGui::BeginDisabled();
        { const float d = 5.0f; changed |= eam::ui::SliderFloat("Seconds kept", &c.recordSeconds, 1.0f, 30.0f, "%.0f s", 0, &d); }
        Tip("How far back a saved recording goes. More seconds take more memory.");
        { float mb = static_cast<float>(c.recordBudgetMb); const float d = 3072.0f;
          if (eam::ui::SliderFloat("Memory for it", &mb, 512.0f, 16384.0f, "%.0f MB", 0, &d)) { c.recordBudgetMb = static_cast<int>(mb / 256.0f + 0.5f) * 256; changed = true; } }
        Tip("The most memory the kept frames may take; past it the oldest go first, so fewer seconds are kept. A 1080p frame takes about 3 to 5 MB "
            "compressed, so 5 seconds at 120 frames a second is about 2 to 3 GB.");
        const Recorder::Status rs = g_recorder.GetStatus();
        if (rs.on && rs.frames) ImGui::TextDisabled("Holding %.1f s: %u frames of %ux%u, %.0f MB%s", rs.seconds, rs.frames, rs.w, rs.h, rs.bytes / 1048576.0,
                                                   rs.missed ? "" : ", none left out");
        else if (rs.on) ImGui::TextDisabled("Waiting for frames (the game must be running and scaled).");
        if (rs.on && rs.missed) ImGui::TextDisabled("%llu frames left out so far (the copies were all busy).", (unsigned long long)rs.missed);
        const bool saving = rs.saving;
        if (saving) ImGui::BeginDisabled();
        if (eam::ui::Button(saving ? "Saving..." : "Save the last seconds", eam::ui::icons::kCheck, eam::ui::ButtonKind::Primary)) SaveRecording();
        if (saving) ImGui::EndDisabled();
        Tip("In the game, Ctrl+Shift + the Save the recording key (see Compare and hotkeys) does the same. The file is named after the game and the time.");
        if (!c.recordOn) ImGui::EndDisabled();
        const std::wstring folder = RecordFolder();
        const int size = WideCharToMultiByte(CP_UTF8, 0, folder.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string folderText(size > 0 ? size - 1 : 0, '\0');
        if (size > 1) WideCharToMultiByte(CP_UTF8, 0, folder.c_str(), -1, folderText.data(), size, nullptr, nullptr);
        ImGui::TextWrapped("Folder: %s", folderText.c_str());
        if (ImGui::SmallButton("Open folder##rec")) { CreateDirectoryW(folder.c_str(), nullptr); ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
        bool ok; const std::string result = g_recorder.LastResult(ok);
        if (!result.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, ok ? eam::ui::theme::V(eam::ui::theme::kAccent) : eam::ui::theme::V(eam::ui::theme::kWarn)); ImGui::TextWrapped("%s", result.c_str()); ImGui::PopStyleColor(); }
    }
    if (!kScalerAddon && eam::ui::SectionHeader("Games (a look per program)")) {
        std::string cur; { std::lock_guard<std::mutex> lk(g_textMutex); cur = g_focusExe; }
        ImGui::Text("Program in focus: %s", cur.empty() ? "(none seen yet)" : cur.c_str());
        Tip("The program that had focus most recently, ignoring Lossless Scaling's own windows. While a game is being scaled this is the game.");
        changed |= ImGui::Checkbox("Switch to a program's saved look when it takes focus", &c.gameAuto);
        Tip("When a program listed below takes focus, its preset is applied automatically (once per change of program, so your own tweaks after that stay). A preset with a different working scale makes the model rebuild for a moment.");
        std::vector<std::string> names; { std::lock_guard<std::mutex> lk(g_settingsMutex); for (auto& pr : g_looks) names.push_back(pr.name); }
        int rmg = -1;
        for (size_t i = 0; i < c.games.size(); ++i) {
            ImGui::PushID((int)i);
            ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(c.games[i].first.c_str()); ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
            if (ImGui::BeginCombo("##gp", c.games[i].second.c_str())) {
                for (auto& n : names) if (ImGui::Selectable(n.c_str(), n == c.games[i].second)) { c.games[i].second = n; changed = true; }
                ImGui::EndCombo();
            }
            ImGui::SameLine(); if (ImGui::SmallButton("Forget")) rmg = (int)i;
            ImGui::PopID();
        }
        if (rmg >= 0) { ForgetGame(g_host, kAddonId, c.games[rmg].first); c.games.erase(c.games.begin() + rmg); changed = true; }
        if (c.games.empty()) ImGui::TextDisabled("No programs yet.");
        static char exeBuf[64] = ""; static int addSel = 0;
        ImGui::InputText("Program (exe name)", exeBuf, sizeof exeBuf);
        Tip("For example WowB.exe. Matching ignores upper and lower case.");
        ImGui::SameLine(); if (ImGui::SmallButton("Use program in focus") && !cur.empty()) snprintf(exeBuf, sizeof exeBuf, "%s", cur.c_str());
        if (addSel >= (int)names.size()) addSel = 0;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::BeginCombo("Look to use", names.empty() ? "(save a preset first)" : names[addSel].c_str())) {
            for (int i = 0; i < (int)names.size(); ++i) if (ImGui::Selectable(names[i].c_str(), i == addSel)) addSel = i;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Add") && exeBuf[0] && !names.empty()) {
            std::string e = exeBuf; for (char& ch : e) ch = (char)tolower((unsigned char)ch);
            e = CleanName(e); bool found = false;
            for (auto& g : c.games) if (g.first == e) { g.second = names[addSel]; found = true; }
            if (!found && !e.empty()) c.games.push_back({ e, names[addSel] });
            changed = true;
        }
        Tip("Use the chosen saved look whenever this program takes focus.");
        if (ImGui::Button("Save the current look for the program in focus") && !cur.empty()) {
            std::string base = cur; const size_t dot = base.rfind('.'); if (dot != std::string::npos && dot > 0) base.resize(dot);
            const std::string n = CleanName(base); const std::string d = LookToText(c.p);
            { std::lock_guard<std::mutex> lk(g_settingsMutex); bool found = false; for (auto& pr : g_looks) if (pr.name == n) { pr.data = d; found = true; } if (!found) g_looks.push_back({ n, d }); }
            bool found = false; for (auto& g : c.games) if (g.first == cur) { g.second = n; found = true; }
            if (!found) c.games.push_back({ cur, n });
            changed = true;
        }
        Tip("Stores everything you have set now (including the HUD areas) as a preset named after the program, and links the program to it.");
    }
    if (eam::ui::SectionHeader("Frame detection (advanced)")) {
        Note("How the addon recognises Lossless Scaling's new real frame. Auto works; change this only if the status stays stuck on waiting.");
        const int modeBefore = c.tapMode;
        if (ImGui::RadioButton("Auto", c.tapMode == 0)) c.tapMode = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Manual", c.tapMode == 1)) c.tapMode = 1;
        tapChanged |= c.tapMode != modeBefore;
        const char* slotNames[] = { "auto (highest)", "S0", "S1", "S2", "S3", "S4", "S5", "S6", "S7" };
        int slot = c.frameSlot + 1;
        if (ImGui::Combo("Frame slot", &slot, slotNames, 9)) { c.frameSlot = slot - 1; tapChanged = true; }
        DispatchSig tick, tap;
        g_tap.GetRoles(tick, tap);
        auto role = [](const char* name, const DispatchSig& sig) {
            ImGui::TextWrapped("%s %s (%u,%u,%u) %s", name, sig.Empty() ? "-" : "", sig.x, sig.y, sig.z, PassText(sig).c_str());
        };
        role("TICK:", tick);
        role("TAP: ", tap);
        if (ImGui::SmallButton("Clear table")) g_tap.ClearTable();
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear roles")) { c.tickSig.clear(); c.tapSig.clear(); tapChanged = true; }
        // the passes seen, busiest first; either can be made the TAP or the TICK by hand
        std::vector<DispatchEntry> passes = g_tap.Snapshot();
        std::sort(passes.begin(), passes.end(), [](const DispatchEntry& a, const DispatchEntry& b) { return a.count > b.count; });
        if (passes.size() > 24) passes.resize(24);
        if (ImGui::BeginTable("passes", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            for (const char* column : { "count", "groups", "views", "auto", "use" }) ImGui::TableSetupColumn(column);
            ImGui::TableHeadersRow();
            for (const DispatchEntry& pass : passes) {
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(pass.key & 0x7fffffff));
                ImGui::TableNextColumn(); ImGui::Text("%u", pass.count);
                ImGui::TableNextColumn(); ImGui::Text("%u,%u,%u", pass.sig.x, pass.sig.y, pass.sig.z);
                ImGui::TableNextColumn(); ImGui::TextWrapped("%s", PassText(pass.sig).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(pass.roleAuto == 1 ? "TICK" : pass.roleAuto == 2 ? "TAP" : "");
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("TAP")) { c.tapSig = pass.sig.Serialize(); c.tapMode = 1; tapChanged = true; }
                ImGui::SameLine();
                if (ImGui::SmallButton("TICK")) { c.tickSig = pass.sig.Serialize(); c.tapMode = 1; tapChanged = true; }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    if (eam::ui::SectionHeader("Technical status")) {
    ImGui::Text("last LS device: %s %s   engine: %s", adapterName.c_str(), hasDisplay ? "(drives a display)" : "(no display output)",
                kScalerAddon ? ScalerEngineText().c_str() : g_engineCardKnown ? "on the LSFG device" : "not started");
    ImGui::Text("frame: %s   NR %.1f ms (avg %.1f)  run %.1f ms   runs %llu   fails %llu", frameInfo.c_str(), g_lastModelMs, g_avgModelMs, g_lastRunMs, (unsigned long long)g_runs, (unsigned long long)g_engine.Stats().fails);
    ImGui::Text("dispatches %llu  ticks %llu  taps %llu  gate:%s  float-slot %d", (unsigned long long)g_tap.Dispatches(), (unsigned long long)g_tap.Ticks(), (unsigned long long)g_tap.Taps(), g_tap.GateName(), g_engine.Stats().floatSlot);
    { std::string tdi; { std::lock_guard<std::mutex> lk(g_textMutex); tdi = g_tappedDeviceText; }
      ImGui::Text("passes seen: %llu   other-adapter passes: %llu   tapped device: %s", (unsigned long long)g_tap.Dispatches(), (unsigned long long)g_otherPasses, tdi.c_str()); }
    }
    if (eam::ui::SectionHeader("Advanced")) {
        int dv = (int)c.p.debugView; const char* views[] = { "Result", "Original", "Delta x4", "Frame role (green real, red generated)", "LSFG flow", "Ghost guard (white = full effect)" };
        if (ImGui::Combo("Diagnostic view", &dv, views, 6)) { c.p.debugView = dv; changed = true; }
        Tip("Shows what the addon is doing instead of the finished picture: the original frame, the model's change amplified 4x, which frames are real or generated, or the motion data. Leave on Result for normal use.");
        changed |= SL("Slow-model watchdog (ms)", &c.watchdogMs, 20.0f, 200.0f, "%.0f");
        Tip("If the model takes longer than this for 30 frames in a row, the addon switches itself off so it can never hurt your frame rate. It re-arms itself after 10 seconds, up to three times per session.");
        char sp[512]; strncpy(sp, c.snippetPath.c_str(), sizeof sp); sp[sizeof sp - 1] = 0;
        if (ImGui::InputText("Model file path (blank = Lossless Scaling folder)", sp, sizeof sp)) { c.snippetPath = sp; changed = true; }
        if (ImGui::IsItemDeactivatedAfterEdit()) { { std::lock_guard<std::mutex> lk(g_settingsMutex); g_config.snippetPath = c.snippetPath; } ScanRequirements(); }
        Tip("Full path to nvngx_dlssnr.dll. Leave blank to use the copy next to LosslessScaling.exe.");
        if (ImGui::SmallButton("Restart engine")) { SwitchOn(); RestartEngine(); }
        Tip("Tear the model down and start it again on the graphics card Lossless Scaling is using.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Dump flow probe (3 frames)")) g_tap.ArmProbe(g_lsDir);
        Tip("Diagnostic: writes the next three frames and Lossless Scaling's motion data to files in its folder.");
        { std::string ps = g_tap.ProbeStatus(); if (ps != "idle") ImGui::TextWrapped("%s", ps.c_str()); }
        Note("present hook: %s, %u presents seen in the process   log: <LS folder>\\logs\\DLSS5NR01.log", PresentHook::Installed() ? "installed" : "not yet", PresentHook::Hits());
    }

    if (changed || createChanged || tapChanged) Commit(c, tapChanged, createChanged);
    if (modelChanged) RestartEngine();   // the DLAA model is chosen when the engine starts
}

} // namespace nr
