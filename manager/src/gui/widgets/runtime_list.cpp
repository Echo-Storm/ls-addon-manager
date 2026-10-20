#include "runtime_list.h"
#include "file_dialog.h"
#include "toast.h"
#include "imgui.h"
#include <eam/icons.h>
#include <eam/widgets.h>
#include <string>
#include <windows.h>

namespace eam {
namespace widgets {

namespace {
std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
} // namespace

RuntimeAction RuntimeListAtBottom(const std::vector<RuntimeFile>& rows, int openMenu) {
    RuntimeAction action;
    if (rows.empty()) return action;
    namespace th = ui::theme;
    const float a = ImGui::GetStyle().Alpha;
    // its height as drawn the frame before (an estimate the first time): the room above it is filled so it ends at the window's bottom
    static float measured = 0.0f;
    const float need = measured > 0.0f ? measured : ImGui::GetFontSize() * 2.0f + rows.size() * (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y);
    const float gap = ImGui::GetContentRegionAvail().y - need - ImGui::GetStyle().ItemSpacing.y;
    if (gap > 0.0f) ImGui::Dummy(ImVec2(0, gap));
    const float top = ImGui::GetCursorPosY();
    ImGui::PushStyleColor(ImGuiCol_Separator, th::V(th::kBorder)); ImGui::Separator(); ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.2f));
    ui::SectionLabel("Runtimes");
    ImGui::PushID("##runtimes");   // its lines are numbered from 0 like the addon cards above them in the same window: their own ID scope
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.15f));
    for (int i = 0; i < (int)rows.size(); ++i) {
        const RuntimeFile& r = rows[i];
        ImGui::PushID(i);
        const float h = ImGui::GetFrameHeight(), w = ImGui::GetContentRegionAvail().x;
        const float buttons = h;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // the line itself: hovering it shows the details
        ImGui::InvisibleButton("##row", ImVec2(w - buttons, h));
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort);
        if (ImGui::IsItemHovered()) dl->AddRectFilled(p, ImVec2(p.x + w - buttons, p.y + h), th::U(th::kRowHover, a), 3.0f);
        const float ico = ImGui::GetFontSize() * 0.9f;
        // a tick: loaded now; a circle: its addon is on and loads it when Lossless Scaling scales a game; a cross: its addon is off (or no file)
        const bool waiting = !r.loaded && r.addonOn && r.exists;
        const char* glyph = r.loaded ? ui::icons::kCheck : waiting ? ui::icons::kCircle : ui::icons::kClose;
        const ImU32 tint = r.loaded ? th::U(th::kAccent, a) : th::U(th::kMuted, a);
        std::string note;
        if (!r.exists) note = "not found";
        else if (!r.read) note = "...";
        else if (r.signature == RuntimeFile::Signature::Broken) note = "modified";
        else if (r.signature != RuntimeFile::Signature::Signed) note = "unsigned";
        ui::svg::Draw(dl, glyph, ImVec2(p.x + 2.0f, p.y + (h - ico) * 0.5f), ico, tint, 2.0f);
        float x = p.x + ico + ImGui::GetFontSize() * 0.55f;
        const float ty = p.y + (h - ImGui::GetFontSize()) * 0.5f, right = p.x + w - buttons - 4.0f;
        const ImVec4 clip(p.x, p.y, right, p.y + h);
        dl->AddText(nullptr, 0.0f, ImVec2(x, ty), th::U(th::kText, a), r.label.c_str(), nullptr, 0.0f, &clip);
        x += ImGui::CalcTextSize(r.label.c_str()).x + ImGui::GetFontSize() * 0.4f;
        const std::string version = r.exists && r.read ? r.ShownVersion() : std::string();
        if (!version.empty()) {
            dl->AddText(nullptr, 0.0f, ImVec2(x, ty), th::U(th::kMuted, a), version.c_str(), nullptr, 0.0f, &clip);
            x += ImGui::CalcTextSize(version.c_str()).x + ImGui::GetFontSize() * 0.4f;
        }
        if (!note.empty()) dl->AddText(nullptr, 0.0f, ImVec2(x, ty), th::U(th::kText, a * 0.75f), note.c_str(), nullptr, 0.0f, &clip);

        if (hovered) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
            ImGui::Text("%s runtime, used by %s (%s)", r.label.c_str(), r.addonName.c_str(), r.addonOn ? "on" : "off");
            if (r.loaded) ImGui::TextColored(th::V(th::kAccent), "Loaded now");
            else if (waiting) ImGui::TextDisabled("Ready: it loads when Lossless Scaling scales a game with %s on", r.addonName.c_str());
            else if (!r.exists) ImGui::TextDisabled("Not loaded: the file is not there");
            else ImGui::TextDisabled("Not loaded: %s is off", r.addonName.c_str());
            if (!r.exists) ImGui::Text("Not found: %s", Utf8(r.path).c_str());
            else if (!r.read) ImGui::TextDisabled("Reading the file...");
            else {
                if (!r.description.empty() || !r.version.empty()) ImGui::TextUnformatted((r.description + (r.description.empty() ? "" : "  ") + r.version).c_str());
                if (r.signature == RuntimeFile::Signature::Signed) ImGui::Text("Signed by %s", r.signer.empty() ? "its maker" : r.signer.c_str());
                else if (r.signature == RuntimeFile::Signature::Broken)
                    ImGui::Text("Modified: signed by %s, then changed, so the signature no longer matches the file", r.signer.empty() ? "its maker" : r.signer.c_str());
                else ImGui::Text("Not signed%s", r.company.empty() ? "" : (" (it says it is " + r.company + "'s)").c_str());
                if (r.shippedKnown) ImGui::TextDisabled(r.shipped ? "The file the addon ships with." : "Not the file the addon ships with.");
                ImGui::TextDisabled("%s", Utf8(r.path).c_str());
                if (!r.sha256.empty()) ImGui::TextDisabled("SHA-256 %s", r.sha256.c_str());
            }
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }

        // + : the files there are for it, one click to switch, and adding one
        ImGui::SameLine(0, 0);
        if (ui::IconButton("##choose", ui::icons::kPlus, h)) { action = { i, RuntimeAction::Choose }; ImGui::OpenPopup("##files"); }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Switch %s file", r.label.c_str());
        if (i == openMenu && !ImGui::IsPopupOpen("##files")) ImGui::OpenPopup("##files");
        // the menu opens beside the list, its bottom at this line (not wherever the pointer is)
        ImGui::SetNextWindowPos(ImVec2(p.x + w + ImGui::GetStyle().ItemSpacing.x, p.y + h), ImGuiCond_Appearing, ImVec2(0.0f, 1.0f));
        if (ImGui::BeginPopup("##files")) {
            ui::SectionLabel((r.label + " file").c_str());
            ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.2f));
            auto line = [&](const RuntimeFile& f, const std::string& name) {
                std::string text = name;
                const std::string v = f.exists && f.read ? f.ShownVersion() : std::string();
                if (!v.empty()) text += "  " + v;
                if (!f.exists) text += "  (missing)";
                else if (f.read && f.signature == RuntimeFile::Signature::Broken) text += "  modified";
                else if (f.read && f.signature != RuntimeFile::Signature::Signed) text += "  unsigned";
                return text;
            };
            const bool usingDefault = r.usingDefault;
            // the default first: always there, so there is always a way back
            {
                const RuntimeFile def = DescribeRuntimeFile(r.defaultPath, r);
                if (ImGui::Selectable(line(def, r.shippedKnown ? "Shipped" : "Default").c_str(), usingDefault)
                    && !usingDefault) {
                    UseRuntimeFile(r, L"");
                    ToastShow(r.label + ": back to the " + (r.shippedKnown ? "shipped" : "default") + " file", ToastType::Success, 4.0f);
                }
            }
            const std::vector<std::wstring> library = RuntimeLibrary(r);
            for (size_t k = 0; k < library.size(); ++k) {
                ImGui::PushID((int)k);
                const RuntimeFile f = DescribeRuntimeFile(library[k], r);
                const bool current = !usingDefault && _wcsicmp(library[k].c_str(), r.path.c_str()) == 0;
                const float trash = ImGui::GetFrameHeight();
                const std::string title = RuntimeFileTitle(library[k]);
                if (ImGui::Selectable((title.empty() ? line(f, r.label) : title + (f.read && f.signature == RuntimeFile::Signature::Signed ? "" : "  unsigned")).c_str(), current,
                                      ImGuiSelectableFlags_DontClosePopups, ImVec2(ImGui::GetFontSize() * 16.0f, 0)) && !current) {
                    UseRuntimeFile(r, library[k]);
                    ToastShow(r.label + ": switched to " + (f.ShownVersion().empty() ? std::string("the new file") : f.ShownVersion()), ToastType::Success, 4.0f);
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::BeginTooltip();
                    if (!f.description.empty()) ImGui::TextUnformatted((f.description + "  " + f.version).c_str());
                    if (f.signature == RuntimeFile::Signature::Signed) ImGui::Text("Signed by %s", f.signer.c_str()); else ImGui::TextUnformatted(f.signature == RuntimeFile::Signature::Broken ? "Modified after signing" : "Not signed");
                    if (!f.sha256.empty()) ImGui::TextDisabled("SHA-256 %s", f.sha256.c_str());
                    ImGui::EndTooltip();
                }
                ImGui::SameLine();
                if (ui::IconButton("##remove", ui::icons::kTrash, trash)) {
                    std::string error;
                    if (RemoveRuntimeFile(r, library[k], error)) ToastShow(r.label + ": file removed (moved to runtimes\\.removed)", ToastType::Info, 4.0f);
                    else ToastShow(error, ToastType::Warning, 5.0f);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Remove it from this list");
                ImGui::PopID();
            }
            ImGui::Separator();
            if (ImGui::Selectable("+  Add a file...")) {
                std::wstring picked;
                const std::wstring title = L"Choose a " + std::wstring(r.label.begin(), r.label.end()) + L" file";
                if (PickOpenFile(title.c_str(), L"DLL files", L"*.dll", picked)) {
                    std::wstring added; std::string error;
                    if (AddRuntimeFile(r, picked, added, error)) {
                        UseRuntimeFile(r, added);
                        const RuntimeFile f = DescribeRuntimeFile(added, r);
                        ToastShow(r.label + ": added and switched" + (f.ShownVersion().empty() ? std::string() : " to " + f.ShownVersion()), ToastType::Success, 4.0f);
                    } else ToastShow(error, ToastType::Warning, 6.0f);
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    measured = ImGui::GetCursorPosY() - top;
    return action;
}

} // namespace widgets
} // namespace eam
