// Offscreen preview of the manager's look: builds the real theme (gui_style.cpp), the real toggle switch, addon card, status bar
// and About tab, the shared widgets.h controls (sliders with defaults, section headers, buttons, SVG icons), and renders them to
// BMP files. No window is created and nothing is drawn on anyone's screen.
//   ui_preview.exe [outDir] [scale] [addon.dll ...]
// Each addon DLL is loaded against a fake host and its settings panel is rendered to preview_<dll name>.bmp, so a panel can
// be looked at (and proven to load against this ImGui) without running Lossless Scaling.
#include <windows.h>
#include <cstdio>
#include <string>
#include <map>
#include <filesystem>
#include <vector>
#include <functional>
#include <algorithm>
#include "imgui.h"
#include "tools/ui_shot.h"
#include "src/gui/gui_style.h"
#include "src/gui/gui_scale.h"
#include "src/gui/widgets/toggle_switch.h"
#include "src/gui/widgets/addon_card.h"
#include "src/gui/widgets/runtime_list.h"
#include "src/addon/addon_manifest.h"
#include "src/gui/widgets/toast.h"
#include "src/gui/widgets/update_offer.h"
#include "src/update/update_check.h"
#include "src/gui/widgets/status_bar.h"
#include "src/gui/widgets/header_bar.h"
#include "src/host/system_stats.h"
#include "src/addon/addon_discovery.h"
#include "src/gui/widgets/empty_state.h"
#include "src/gui/window/status_text.h"
#include "src/gui/tabs/tab_about.h"
#include "src/gui/tabs/tab_features.h"
#include "src/config/config_manager.h"
#include "src/gui/tabs/tab_logs.h"
#include "src/log/logger.h"
#include "eam/version.h"
#include "src/gui/tabs/tab_performance.h"
#include "src/gui/tabs/tab_settings.h"
#include "src/gui/gui_manager.h"
#include "src/addon/addon_manager.h"
#include "src/host/metrics.h"
#include "src/host/gpu_stats.h"
#include "eam/widgets.h"
#include "eam/addon_sdk.h"

using namespace eam;

// The preview does not link the real manager window or addon manager: the few things the Settings tab touches are stubbed.
void GuiManager::ApplyHotkey() {}
const char* GuiManager::HotkeyStatus() { return "registered"; }
void GuiManager::RequestUserScale() {}
std::vector<AddonInfo>& AddonManager::GetAddons() { static std::vector<AddonInfo> v; return v; }

static void Shell(const char* active, const std::string& status, const std::function<void()>& body) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##Main", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    widgets::HeaderBar();
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::BeginChild("##content", ImVec2(0, -widgets::StatusBarHeight() - 2.0f), false, ImGuiWindowFlags_NoBackground);
    if (ImGui::BeginTabBar("##MainTabs")) {
        for (const char* t : { "Addons", "Features", "Performance", "Settings", "Logs", "About" }) {
            ImGuiTabItemFlags f = (std::string(t) == active) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(t, nullptr, f)) { if (std::string(t) == active) body(); ImGui::EndTabItem(); }
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    widgets::StatusBar(status);
    ImGui::End();
    widgets::ToastRender();
}

struct FakeHost : IHost {
    std::map<std::string, std::string> cfg;
    void Log(EamLogLevel, const char* m) override { printf("[addon] %s\n", m); }
    const char* GetConfig(const char* id, const char* k, const char* d) override { auto it = cfg.find(std::string(id) + "/" + k); return it == cfg.end() ? d : it->second.c_str(); }
    void SetConfig(const char* id, const char* k, const char* v) override { cfg[std::string(id) + "/" + k] = v; }
    void SaveConfig() override {}
    uint32_t GetHostVersion() override { return EAM_API_VERSION_INT; }
    void SubscribeEvent(uint32_t, EamEventCallback, void*) override {}
    void UnsubscribeEvent(uint32_t, EamEventCallback) override {}
    void PublishEvent(uint32_t, const void*, uint32_t) override {}
    void* GetD3D11Device() override { return nullptr; }
    void* GetD3D11DeviceContext() override { return nullptr; }
    void SetPreDispatchCallback(EamPreDispatchCallback, void*) override {}
    void SetPostDispatchCallback(EamPostDispatchCallback, void*) override {}
    void* GetCurrentComputeShader() override { return nullptr; }
    uint32_t GetDispatchCount() override { return 0; }
    void* GetDispatchingContext() override { return nullptr; }
    void* CreateImage(const void*, uint32_t, uint32_t, uint32_t) override { return nullptr; }
    void ReleaseImage(void*) override {}
    void SetStatus(const char*, const char*, int) override {}
    void PublishMetric(const char*, const char*, double, const char*) override {}
};

int main(int argc, char** argv) {
    std::string out = argc > 1 ? argv[1] : ".";
    const float scale = argc > 2 ? (float)atof(argv[2]) : 1.0f;
    ImGui::CreateContext();
    LoadUiFonts("", "");
    ApplyUiScale(scale);

    UiShot shot;
    const int W = (int)(1100 * scale), H = (int)(740 * scale);   // a window of a realistic size (the README's screenshots)
    if (!shot.Init(W, H)) { printf("device init failed\n"); return 1; }

    // EAM_PREVIEW_CLEAN=1 draws the tidy scene used for the README screenshots: no toast, only the first addon on, no error chips.
    char* cleanEnv = nullptr; size_t cleanLen = 0; _dupenv_s(&cleanEnv, &cleanLen, "EAM_PREVIEW_CLEAN");
    const bool clean = cleanEnv != nullptr; free(cleanEnv);
    // the three plugins as a real install has them, with their own icon.svg files from the repository (tools\..\addons\DLSS5NR01)
    std::filesystem::path root;
    { wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH); root = std::filesystem::path(exe).parent_path().parent_path().parent_path().parent_path(); }
    const std::filesystem::path plugins = root / "addons" / "DLSS5NR01";
    AddonInfo a, b, c;
    a.id = "DLSS5NR01"; a.manifest.name = "DLSS 5 Neural Rendering"; a.manifest.version = EAM_VERSION_STRING; a.manifest.author = "Echo-Storm"; a.hModule = (HMODULE)1; a.enabled = true;
    b.id = "DLSS4DLAA"; b.manifest.name = "DLSS Upscaler"; b.manifest.version = EAM_VERSION_STRING; b.manifest.author = "Echo-Storm"; b.enabled = false;
    c.id = "FSR3UPSC"; c.manifest.name = "FSR Upscaler"; c.manifest.version = EAM_VERSION_STRING; c.manifest.author = "Echo-Storm"; c.hModule = (HMODULE)1; c.enabled = true;
    a.security = b.security = c.security = SecurityVerdict::Trusted;   // as a real install of our own addons shows them
    if (!clean) { b.enabled = true; b.faulted = true; }   // the other scene shows the ERROR state
    ReadSvgIcon(plugins / "icon.svg", a.iconSvg, a.iconSvgView);
    ReadSvgIcon(plugins / "products" / "DLSS4DLAA" / "icon.svg", b.iconSvg, b.iconSvgView);
    ReadSvgIcon(plugins / "products" / "FSR3UPSC" / "icon.svg", c.iconSvg, c.iconSvgView);
    // the runtimes, as the repository's addon.json files list them, read from the install in D:\Utilities\Lossless Scaling (as the real
    // list reads them: version, signature, SHA-256). EAM_PREVIEW_FSR=<file>: that file as the FSR runtime (only read, never loaded)
    const std::wstring lsDir = L"D:\\Utilities\\Lossless Scaling";
    // (the model file is the person's own: the tidy scene for the README leaves its line out rather than show this machine's copy)
    { AddonManifest m; if (!clean && ReadManifest(plugins / "addon.json", m)) a.manifest.runtimes = m.runtimes; }
    { AddonManifest m; if (ReadManifest(plugins / "products" / "DLSS4DLAA" / "addon.json", m)) b.manifest.runtimes = m.runtimes; }
    { AddonManifest m; if (ReadManifest(plugins / "products" / "FSR3UPSC" / "addon.json", m)) c.manifest.runtimes = m.runtimes; }
    a.dllPath = lsDir + L"\\addons\\DLSS5NR01\\DLSS5NR01.dll";
    b.dllPath = lsDir + L"\\addons\\DLSS4DLAA\\DLSS4DLAA.dll";
    c.dllPath = lsDir + L"\\addons\\FSR3UPSC\\FSR3UPSC.dll";
    {
        char* v = nullptr; size_t n = 0; _dupenv_s(&v, &n, "EAM_PREVIEW_FSR");
        if (v && *v && !c.manifest.runtimes.empty()) c.manifest.runtimes[0].file = v;
        free(v);
    }
    const std::vector<AddonInfo> runtimeAddons = { a, b, c };
    for (int i = 0; i < 100; ++i) {   // the files are read on a thread of their own: wait for them, so the picture shows what they are
        const std::vector<RuntimeFile> rows = RuntimeFiles(runtimeAddons, lsDir);
        bool all = std::all_of(rows.begin(), rows.end(), [](const RuntimeFile& r) { return r.read || !r.exists; });
        for (const RuntimeFile& r : rows) for (const std::wstring& lib : RuntimeLibrary(r)) all = all && DescribeRuntimeFile(lib, r).read;   // the + menus' files too
        if (all) break;
        Sleep(100);
    }

    float model = 0.5f, sharpen = 0.0f, vib = 1.2f, blend = 0.72f, gamma = 1.0f; int passes = 1, grain = 2; bool sw = true, sw2 = false; int sel = 0;
    const float dModel = 0.35f, dSharpen = 0.0f, dVib = 0.0f, dBlend = 1.0f, dGamma = 1.0f; const int dPasses = 1, dGrain = 1;
    if (!clean) widgets::ToastShow("Installed 'Cool Addon' (switched off). Turn it on with its switch.", widgets::ToastType::Success, 1000.0f);
    std::string status = window::StatusCounts(3, 2);

    // live data for the cards and the Performance tab: 20 s of a game near 60 fps with a few hitches, a model at ~6.6 ms, a GPU at its cap
    {
        Metrics& M = Metrics::Instance();
        const double now = M.Now();
        unsigned seed = 12345;
        auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / (float)(1 << 24); };
        for (double t = now - 20.0; t <= now; t += 1.0 / 60.0) {
            float ms = 16.7f + (rnd() - 0.5f) * 1.6f;
            if (rnd() > 0.985f) ms += 9.0f + rnd() * 14.0f;   // a hitch now and then
            M.PublishAt("DLSS5NR01", "frame_ms", ms, "ms", t);
        }
        for (double t = now - 20.0; t <= now; t += 0.2) {
            M.PublishAt("DLSS5NR01", "model_ms", 6.6f + (rnd() - 0.5f) * 0.7f, "ms", t);
            M.PublishAt("DLSS5NR01", "keepup_pct", 98.0f + rnd() * 2.0f, "%", t);
        }
        for (double t = now - 20.0; t <= now; t += 0.5) {
            const float u = 90.0f + (rnd() - 0.3f) * 9.0f;
            M.PublishAt("system", "gpu_util", u > 100 ? 100 : u, "%", t);
            M.PublishAt("system", "gpu_power_w", 281.0f + (rnd() - 0.5f) * 4.0f, "W", t);
        }
        M.SetStatus("DLSS5NR01", "Running, model 5.2 ms, keeps up 99%", 1);
        M.SetStatus("FSR3UPSC", "FSR 3.1.4 1920x1080 -> 3840x2160, 1.6 ms", 1);
        SystemStats::Snapshot sys; sys.ok = true; sys.cpuPercent = 23; sys.ramUsedMB = 18841; sys.ramTotalMB = 32703;
        SystemStats::Instance().InjectForPreview(sys);
        GpuStats::Snapshot g; g.name = "NVIDIA GeForce RTX 4070 Ti SUPER"; g.driver = "616.92"; g.deviceCount = 1; g.utilGpu = 97; g.utilMem = 44;
        g.clockGraphics = 2610; g.clockMem = 10501; g.tempC = 68; g.powerW = 283.4; g.powerLimitW = 285.0; g.vramUsedMB = 13132; g.vramTotalMB = 16376; g.throttle = 0x4;
        GpuStats::Instance().InjectForPreview(g);
    }

    // the addon panels (each DLL given on the command line), keyed by file name without .dll
    std::map<std::string, std::function<void()>> panels;
    static FakeHost host;
    // Neural Rendering's model file, where a real install keeps it (the panel then shows its requirements met); nothing is loaded from it here
    if (GetFileAttributesW(L"D:\\Utilities\\Lossless Scaling\\nvngx_dlssnr.dll") != INVALID_FILE_ATTRIBUTES)
        host.cfg["DLSS5NR01/snippetPath"] = "D:\\Utilities\\Lossless Scaling\\nvngx_dlssnr.dll";
    {   // EAM_PREVIEW_CFG="FSR3UPSC/scalerStability=0.4;...": settings the addons start with (for a picture with the sliders somewhere)
        char* v = nullptr; size_t n = 0; _dupenv_s(&v, &n, "EAM_PREVIEW_CFG");
        for (std::string rest = v ? v : ""; !rest.empty();) {
            const size_t semi = rest.find(';');
            const std::string item = rest.substr(0, semi);
            if (const size_t eq = item.find('='); eq != std::string::npos) host.cfg[item.substr(0, eq)] = item.substr(eq + 1);
            rest = semi == std::string::npos ? std::string() : rest.substr(semi + 1);
        }
        free(v);
        // "DLSS5NR01/_enabled=0": Neural Rendering switched off in the manager, its card and the status bar with it
        if (host.cfg["DLSS5NR01/_enabled"] == "0") { a.enabled = false; a.hModule = nullptr; status = window::StatusCounts(3, 1); }
    }
    for (int i = 3; i < argc; ++i) {
        HMODULE h = LoadLibraryA(argv[i]);
        if (!h) { printf("cannot load %s (error %lu)\n", argv[i], GetLastError()); continue; }
        using Init_t = void (*)(IHost*, ImGuiContext*, void*, void*, void*); using Void_t = void (*)();
        auto init = (Init_t)GetProcAddress(h, "AddonInitialize"); auto render = (Void_t)GetProcAddress(h, "AddonRenderSettings");
        if (!init || !render) { printf("%s has no panel exports\n", argv[i]); continue; }
        ImGuiMemAllocFunc af; ImGuiMemFreeFunc ff; void* ud; ImGui::GetAllocatorFunctions(&af, &ff, &ud);
        init(&host, ImGui::GetCurrentContext(), (void*)af, (void*)ff, ud);
        std::string base = argv[i]; const size_t sl = base.find_last_of("/\\"); if (sl != std::string::npos) base = base.substr(sl + 1);
        if (base.size() > 4) base = base.substr(0, base.size() - 4);
        panels[base] = render;
    }

    // 1. Addons tab: the slim list at the left, the selected addon at the right with its own panel (as tab_addons.cpp lays it out)
    char search[64] = {};
    auto env = [](const char* name) { char* v = nullptr; size_t n = 0; _dupenv_s(&v, &n, name); std::string r = v ? v : ""; free(v); return r; };
    const std::string previewScroll = env("EAM_PREVIEW_SCROLL");
    std::vector<std::string> previewOpen;
    for (std::string rest = env("EAM_PREVIEW_OPEN"); !rest.empty();) {
        const size_t bar = rest.find('|');
        previewOpen.push_back(rest.substr(0, bar));
        rest = bar == std::string::npos ? std::string() : rest.substr(bar + 1);
    }
    auto addonsTab = [&](int selected) {
        AddonInfo* list[3] = { &a, &b, &c };
        AddonInfo& chosen = *list[selected];
        Shell("Addons", status, [&] {
            ImGui::Dummy(ImVec2(0, 5));
            const float avail = ImGui::GetContentRegionAvail().x;
            const float listWidth = std::clamp(avail * 0.26f, S(220.0f), S(280.0f));
            ImGui::BeginChild("AddonList", ImVec2(listWidth, -1), false);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##addon_search", "Search (Ctrl+F)", search, sizeof search);
            ImGui::Dummy(ImVec2(0, S(2)));
            eam::ui::Button("Install addon", eam::ui::icons::kDownload, eam::ui::ButtonKind::Primary); ImGui::SameLine();
            eam::ui::IconButton("##open_folder", eam::ui::icons::kFolder, ImGui::GetFrameHeight());
            ImGui::Dummy(ImVec2(0, S(6)));
            ImGui::PushStyleColor(ImGuiCol_Separator, eam::ui::theme::V(eam::ui::theme::kBorder)); ImGui::Separator(); ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, S(4)));
            for (int i = 0; i < 3; ++i) { widgets::AddonCard(*list[i], i, i == selected); ImGui::Dummy(ImVec2(0, S(1))); }
            {   // as tab_addons.cpp: the runtimes at the bottom of the list
                std::vector<RuntimeFile> rows = RuntimeFiles({ a, b, c }, lsDir);   // the addons as the scene has them now (on or off)
                for (RuntimeFile& r : rows) r.loaded = r.addonOn && r.exists && r.addonId == "DLSS5NR01";   // the preview loads none: one running, one waiting, one off
                // EAM_PREVIEW_RUNTIME_MENU=<line>: that line's + menu shown open
                char* mv = nullptr; size_t mn = 0; _dupenv_s(&mv, &mn, "EAM_PREVIEW_RUNTIME_MENU");
                const int openMenu = mv && *mv ? atoi(mv) : -1; free(mv);
                widgets::RuntimeListAtBottom(rows, openMenu);
            }
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("AddonDetail", ImVec2(0, -1), true);
            const float iconSize = S(44.0f);
            widgets::DrawAddonIcon(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), iconSize, chosen, chosen.enabled);
            ImGui::Dummy(ImVec2(iconSize, iconSize)); ImGui::SameLine(0, S(12));
            ImGui::BeginGroup();
            if (ImFont* t = TitleFont()) ImGui::PushFont(t, ImGui::GetStyle().FontSizeBase * 1.2f);
            ImGui::TextUnformatted(chosen.manifest.name.c_str());
            if (TitleFont()) ImGui::PopFont();
            ImGui::TextDisabled("v%s  \xc2\xb7  %s", chosen.manifest.version.c_str(), chosen.manifest.author.c_str());
            ImGui::EndGroup();
            const float switchWidth = ImGui::GetFrameHeight() * 0.8f * 1.8f;
            ImGui::SameLine(ImGui::GetWindowWidth() - switchWidth - ImGui::GetStyle().WindowPadding.x - S(4));
            bool on = chosen.enabled; widgets::ToggleSwitch("##detail_enable", &on);
            ImGui::Dummy(ImVec2(0, S(2)));
            ImGui::TextDisabled("Status"); ImGui::SameLine();
            ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kAccent), "Loaded"); ImGui::SameLine(0, S(24));
            ImGui::TextDisabled("Security"); ImGui::SameLine();
            ImGui::TextColored(eam::ui::theme::V(eam::ui::theme::kAccent), "Trusted");
            {
                const float rw = ImGui::CalcTextSize("Remove").x + ImGui::GetFontSize() * 3.2f;
                ImGui::SameLine(ImGui::GetWindowWidth() - rw - ImGui::GetStyle().WindowPadding.x - S(4));
                eam::ui::Button("Remove", eam::ui::icons::kTrash, eam::ui::ButtonKind::Danger);
            }
            ImGui::Dummy(ImVec2(0, S(4)));
            if (ImGui::BeginTabBar("##detail_tabs")) {
                if (ImGui::BeginTabItem("Settings", nullptr, ImGuiTabItemFlags_SetSelected)) {
                    // EAM_PREVIEW_OPEN="Motion|Upscaling": sections of the panel to show open; EAM_PREVIEW_SCROLL=<px>: how far down the panel starts
                    // (both for a picture that shows the settings rather than the status; nothing else uses them)
                    if (!previewScroll.empty() && selected == 2) ImGui::SetNextWindowScroll(ImVec2(0.0f, (float)atof(previewScroll.c_str())));
                    ImGui::BeginChild("##addon_settings", ImVec2(0, 0), false);
                    for (const std::string& section : previewOpen) ImGui::GetStateStorage()->SetInt(ImGui::GetID(section.c_str()), 1);
                    auto it = panels.find(chosen.id);
                    if (it != panels.end()) it->second(); else ImGui::TextDisabled("(its panel was not given to the preview)");
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Overview")) ImGui::EndTabItem();
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
        });
    };
    shot.Frame([&] { addonsTab(0); }, 12);
    shot.Save((out + "/preview_addons.bmp").c_str());
    shot.Frame([&] { addonsTab(2); }, 12);
    shot.Save((out + "/preview_upscaler.bmp").c_str());

    // 1b. The Remove confirmation over the Addons tab, and the empty state
    shot.Frame([&] {
        Shell("Addons", status, [&] {
            ImGui::Dummy(ImVec2(0, 5));
            eam::ui::Button("Install addon", eam::ui::icons::kDownload, eam::ui::ButtonKind::Primary); ImGui::SameLine();
            eam::ui::Button("Open addons folder", eam::ui::icons::kFolder);
            ImGui::Dummy(ImVec2(0, 6));
            widgets::AddonCard(a, 0, true); widgets::AddonCard(b, 1, false);
            static bool opened = false;
            if (!opened) { ImGui::OpenPopup("Remove addon"); opened = true; }
            if (ImGui::BeginPopupModal("Remove addon", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
                ImGui::TextWrapped("Remove %s?", a.manifest.name.c_str());
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::TextWrapped("It is switched off and its folder is moved to addons/.removed (nothing is erased; move the folder back to restore it). Its settings are kept, so installing it again brings them back.");
                ImGui::PopTextWrapPos();
                ImGui::Dummy(ImVec2(0, 6));
                eam::ui::Button("Remove", eam::ui::icons::kTrash, eam::ui::ButtonKind::Danger); ImGui::SameLine(); eam::ui::Button("Cancel", eam::ui::icons::kClose);
                ImGui::EndPopup();
            }
        });
    }, 14);
    shot.Save((out + "/preview_remove.bmp").c_str());
    shot.Frame([&] {
        Shell("Addons", status, [&] {
            ImGui::Dummy(ImVec2(0, 5));
            eam::ui::Button("Install addon", eam::ui::icons::kDownload, eam::ui::ButtonKind::Primary); ImGui::SameLine();
            eam::ui::Button("Open addons folder", eam::ui::icons::kFolder);
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::BeginChild("AddonList", ImVec2(260 * ImGui::GetStyle().FontScaleDpi, -1), false);
            widgets::EmptyState(260 * ImGui::GetStyle().FontScaleDpi, "No addons installed yet", "Use Install addon above, or drop a folder, .zip or .dll onto this window.");
            ImGui::EndChild();
        });
    }, 12);
    shot.Save((out + "/preview_empty.bmp").c_str());

    // 2. Controls: section headers, sliders (default tick, modified ring, hover), buttons, icons
    ImVec2 hoverAt(-1, -1);
    auto controls = [&] {
        Shell("Settings", status, [&] {
            ImGui::Dummy(ImVec2(0, 5));
            if (eam::ui::SectionHeader("Quality and performance", true)) {
                eam::ui::SliderFloat("Model resolution", &model, 0.25f, 1.0f, "%.2f x the frame", 0, &dModel);
                eam::ui::SliderInt("Model passes", &passes, 1, 4, "%d", 0, &dPasses);
                eam::ui::SliderFloat("Blend amount", &blend, 0.0f, 2.0f, "%.2f", 0, &dBlend);
                hoverAt = ImVec2(ImGui::GetItemRectMin().x + 160.0f * ImGui::GetStyle().FontScaleDpi, ImGui::GetItemRectMin().y + ImGui::GetFrameHeight() * 0.5f);
                eam::ui::SliderFloat("Sharpen", &sharpen, 0.0f, 1.0f, sharpen <= 0.001f ? "off" : "%.2f", 0, &dSharpen);
            }
            if (eam::ui::SectionHeader("Picture (sharpness, tone, colour, grain)", true)) {
                eam::ui::SliderFloat("Vibrance", &vib, 0.0f, 2.0f, "%.2f", 0, &dVib);
                eam::ui::SliderFloat("Gamma", &gamma, 0.5f, 2.0f, fabsf(gamma - 1.0f) < 0.002f ? "unchanged" : "%.2f", 0, &dGamma);
                eam::ui::SliderInt("Grain size", &grain, 1, 4, "%d px", 0, &dGrain);
            }
            eam::ui::SectionHeader("Keep the HUD untouched");
            eam::ui::SectionHeader("Games (a look per program)");
            ImGui::Dummy(ImVec2(0, 8));
            eam::ui::SectionLabel("Buttons and icons");
            eam::ui::Button("Save preset", eam::ui::icons::kCheck, eam::ui::ButtonKind::Primary); ImGui::SameLine();
            eam::ui::Button("Restore defaults", eam::ui::icons::kReset); ImGui::SameLine();
            eam::ui::Button("Remove", eam::ui::icons::kClose, eam::ui::ButtonKind::Danger); ImGui::SameLine();
            eam::ui::Button("Learn more", eam::ui::icons::kExternal, eam::ui::ButtonKind::Flat);
            ImGui::Dummy(ImVec2(0, 4));
            for (const char* ic : { eam::ui::icons::kHeart, eam::ui::icons::kPlus, eam::ui::icons::kDownload, eam::ui::icons::kFolder, eam::ui::icons::kReset, eam::ui::icons::kCheck,
                                    eam::ui::icons::kClose, eam::ui::icons::kExternal, eam::ui::icons::kInfo, eam::ui::icons::kCoffee, eam::ui::icons::kPower, eam::ui::icons::kPackage,
                                    eam::ui::icons::kSliders, eam::ui::icons::kEye }) {
                ImGui::PushID(ic); eam::ui::IconButton("##i", ic, ImGui::GetFrameHeight() * 1.2f); ImGui::PopID(); ImGui::SameLine();
            }
            ImGui::NewLine();
            {   // filled hearts at three sizes (the Ko-fi heart is a filled path)
                const ImVec2 hp = ImGui::GetCursorScreenPos();
                for (int i = 0; i < 3; ++i) {
                    const float sz = i == 0 ? 15.0f : (i == 1 ? 32.0f : 64.0f) ;
                    eam::ui::svg::Draw(ImGui::GetWindowDrawList(), eam::ui::icons::kHeart, ImVec2(hp.x + i * 90.0f, hp.y), sz * ImGui::GetStyle().FontScaleDpi, eam::ui::theme::U(eam::ui::theme::kAccent), 0.0f, eam::ui::theme::U(eam::ui::theme::kAccent));
                }
                ImGui::Dummy(ImVec2(10, 70.0f * ImGui::GetStyle().FontScaleDpi));
            }
            widgets::ToggleSwitch("##t1", &sw); ImGui::SameLine(); ImGui::TextUnformatted("Enabled");
            widgets::ToggleSwitch("##t2", &sw2); ImGui::SameLine(); ImGui::TextUnformatted("Compare view");
        });
    };
    shot.Frame(controls, 12);
    shot.Frame(controls, 6, hoverAt.x, hoverAt.y);   // second pass: the pointer rests on "Blend amount"
    shot.Save((out + "/preview_settings.bmp").c_str());

    // 2b. Performance tab (the real one, fed the synthetic data above)
    {
        ImGuiStyle& st = ImGui::GetStyle(); (void)st;
        GpuStats::Snapshot g = GpuStats::Instance().Get(); GpuStats::Instance().InjectForPreview(g);   // keep it fresh
        shot.Frame([&] { GpuStats::Instance().InjectForPreview(g); Shell("Performance", status, [&] { RenderTabPerformance(); }); }, 12);
        shot.Save((out + "/preview_performance.bmp").c_str());
    }

    // 2c. Settings tab (the real one; the config is empty, so everything shows its defaults)
    shot.Frame([&] { Shell("Settings", status, [&] { RenderTabSettings(nullptr); }); }, 12);
    shot.Save((out + "/preview_settings_tab.bmp").c_str());

    // 3. Features tab (the real one): ReShade passthrough on, so its options show; the tidy scene leaves Windowed off, the other one turns it on
    //    too, which shows the "restart to apply" label (the hooks are not in, in a preview)
    {
        ConfigManager& cfg = ConfigManager::Instance();
        cfg.Load((std::filesystem::path(out) / "preview_config.json").wstring());
        cfg.SetAddonEnabled("ReShadePassthrough", true);
        cfg.SetAddonEnabled("WindowedMode", !clean);
        shot.Frame([&] { Shell("Features", status, [&] { RenderTabFeatures(); }); }, 12);
        shot.Save((out + "/preview_features.bmp").c_str());
    }

    // 3b. About tab (the real one)
    shot.Frame([&] { Shell("About", status, [&] { RenderTabAbout(); }); }, 12);
    shot.Save((out + "/preview_about.bmp").c_str());

    // 3c. The update offer, over the About tab: a newer release as if the check had found it (nothing is downloaded)
    {
        update::Status st;
        st.state = update::State::Available;
        st.current = EAM_VERSION_STRING;
        st.latest = "0.9.9";
        st.url = std::string(update::kReleasesPage) + "/tag/v0.9.9";
        st.checkedAt = 1;   // the window opens once per check
        st.zipUrl = st.url;
        st.notes = update::PlainNotes("HDR games in the DLSS and FSR Upscalers, before / after pictures, and addons that can no longer freeze Lossless Scaling.\n\n"
                                      "- **HDR games in the DLSS and FSR Upscalers.** 10-bit and 16-bit frames are upscaled in their SDR view and go back as HDR.\n"
                                      "- **A stuck runtime can no longer freeze Lossless Scaling.** NVIDIA's and AMD's code runs on threads of its own.\n"
                                      "- **Before / after pictures** (Ctrl+Shift+F4): the upscaled picture and NIS's, side by side.\n"
                                      "- **Saying why nothing happens**: a card that is not NVIDIA's, a window layout, a frame format.\n"
                                      "- The [changelog](https://example.invalid) has the rest.");
        update::SetStatusForTest(st);
        shot.Frame([&] { Shell("About", status, [&] { RenderTabAbout(); widgets::UpdateOffer(); }); }, 12);
        shot.Save((out + "/preview_update.bmp").c_str());
        update::SetStatusForTest(update::Status());
    }

    // 4. Logs tab, with a few typical lines (sample data, like the rest of the preview)
    {
        LOG_INFO("Core", "%s v%s starting...", EAM_PRODUCT_NAME, EAM_VERSION_STRING);
        LOG_INFO("GUI", "Tray icon added");
        LOG_INFO("AddonManager", "Found 1 addons in addons");
        LOG_INFO("AddonManager", "Loaded 'DLSS5NR01' %s", "0.9.1");
        LOG_INFO("Features", "ReShade input passthrough is on");
        LOG_INFO("GUI", "Hotkey Ctrl+Shift+F12 registered");
        LOG_INFO("DLSS5NR01", "Engine started on the LSFG device");
        shot.Frame([&] { Shell("Logs", status, [&] { RenderTabLogs(); }); }, 12);
        shot.Save((out + "/preview_logs.bmp").c_str());
    }

    shot.Shutdown();
    printf("wrote previews to %s\n", out.c_str());
    return 0;
}
