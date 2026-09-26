#include "gui_manager.h"
#include "gui_scale.h"
#include "gui_style.h"
#include "icon_loader.h"
#include "tabs/tab_addons.h"
#include "widgets/toast.h"
#include "window/chrome.h"
#include "window/dpi.h"
#include "window/hotkey.h"
#include "window/main_frame.h"
#include "window/placement.h"
#include "window/tray.h"
#include "window/window_device.h"
#include "../addon/addon_manager.h"
#include "../config/config_manager.h"
#include "../host/gpu_stats.h"
#include "../log/logger.h"
#include "../update/update_check.h"
#include "../../sdk/include/eam/version.h"
#include "imgui.h"
#include "imgui_internal.h"   // ImGui::ErrorRecoveryStoreState / ErrorRecoveryTryToRecoverState
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <shellapi.h>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace eam {

namespace {

constexpr const wchar_t* kWindowClass = L"LSAddonManagerClass";
constexpr UINT_PTR kTrayTimer = 0x4C52;     // the tray icon could not be added (Explorer busy or just restarted): try again shortly
constexpr UINT kTrayRetryMs = 2000;
constexpr int kTrayTries = 30;
constexpr UINT_PTR kUpdateTimer = 0x4C51;   // once a minute: is the daily update check due, and is there something to announce?

// Everything the window and its thread share. One window per process. It is allocated once and never destroyed: static destructors run
// under the loader lock when the DLL unloads, which is no place to release D3D objects.
struct Shell {
    AddonManager* manager = nullptr;
    std::function<void()> beforeAddons;   // run first on the window thread (see StartGuiThread)
    HWND hwnd = nullptr;                    // set once the window and its device exist
    std::atomic<bool> hidden{ false };      // closed to the tray: the window exists but is not shown, and nothing is drawn
    std::atomic<int> failFramesForTest{ 0 };
    int frameFailures = 0;                  // frames whose drawing threw, this session
    std::atomic<bool> scaleDirty{ false };  // the interface-size setting changed: apply it before the next frame
    bool minimized = false;
    bool bringAddonsForward = false;        // set by a drop: show the Addons tab so the confirmation is seen
    float displayScale = 1.0f;              // what Windows asks for (monitor dpi / 96)
    int trayTries = 0;                      // failed attempts to put the icon in the tray since it was last wanted
    UINT taskbarCreated = 0;                // broadcast when Explorer restarts and the tray icons have to be added again
    HICON trayIcon = nullptr;
    window::WindowDevice device;
    window::SavedPlacement saved;           // captured when the window is closed, written to the settings when the thread winds down
};
Shell& g = *new Shell;

void ShowManager(bool show) {
    if (!g.hwnd) return;
    if (show) {
        ShowWindow(g.hwnd, IsIconic(g.hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(g.hwnd);
        g.hidden = false;
    } else {
        ShowWindow(g.hwnd, SW_HIDE);
        g.hidden = true;
    }
}

void PersistPlacement() {
    if (!g.saved.valid) return;
    auto& cfg = ConfigManager::Instance();
    cfg.GlobalSet("ui", "window", window::ToJson(g.saved));
    cfg.Save();
}

float TotalScale() { return g.displayScale * window::UserScale(); }

std::wstring WindowTitle() {
    std::wstring title = EAM_PRODUCT_NAME_W L" v";
    for (const char* c = EAM_VERSION_STRING; *c; ++c) title += static_cast<wchar_t>(*c);
    return title;
}

// Closing hides the window instead of ending the GUI thread: the addons keep their ImGui context, and the manager can be reopened from
// the tray icon or the hotkey. The first time, a balloon says so.
void OnClose(HWND hwnd) {
    const window::SavedPlacement now = window::CapturePlacement(hwnd, g.displayScale);
    if (now.valid) g.saved = now;
    PersistPlacement();
    ShowManager(false);

    auto& cfg = ConfigManager::Instance();
    if (cfg.GlobalGetOr<bool>("ui", "hide_hint_shown", false)) return;
    std::wstring text = L"Click its icon in the notification area";
    if (window::hotkey::Registered()) text += L" or press " + window::hotkey::LabelFromConfig();
    text += L" to open it again.";
    window::tray::Balloon(L"The addon manager is still running", text.c_str());
    cfg.GlobalSet("ui", "hide_hint_shown", true);
    cfg.Save();
}

// Put the icon in the tray; when Windows says no, try again every couple of seconds for a minute (a restarted Explorer often is not ready at once).
void EnsureTrayIcon(HWND hwnd) {
    if (window::tray::Added()) { KillTimer(hwnd, kTrayTimer); g.trayTries = 0; return; }
    if (window::tray::Add(hwnd, g.trayIcon, window::hotkey::Registered() ? window::hotkey::LabelFromConfig() : L"")) { KillTimer(hwnd, kTrayTimer); g.trayTries = 0; return; }
    if (++g.trayTries < kTrayTries) SetTimer(hwnd, kTrayTimer, kTrayRetryMs, nullptr);
    else KillTimer(hwnd, kTrayTimer);
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;

    if (g.taskbarCreated && msg == g.taskbarCreated) {   // Explorer restarted: its tray forgot our icon
        window::tray::Forget();
        g.trayTries = 0;
        EnsureTrayIcon(hwnd);
        return 0;
    }

    switch (msg) {
    case WM_SIZE:
        g.minimized = (wParam == SIZE_MINIMIZED);
        if (!g.minimized) g.device.Resize(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_DROPFILES: {   // an addon (folder, .zip or .dll) dropped on the window: ask before installing it
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        wchar_t path[2048];
        if (DragQueryFileW(drop, 0, path, 2048)) { RequestInstallFromPath(path); g.bringAddonsForward = true; }
        DragFinish(drop);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = static_cast<LONG>(S(static_cast<float>(window::kMinLogicalW)));
        info->ptMinTrackSize.y = static_cast<LONG>(S(static_cast<float>(window::kMinLogicalH)));
        return 0;
    }

    case WM_DPICHANGED: {
        // Moved to a monitor with a different scale: rebuild the style at the new scale (fonts are rasterised at the final size, so no
        // atlas rebuild is needed) and adopt the rectangle Windows suggests so the window keeps its apparent size.
        g.displayScale = HIWORD(wParam) / 96.0f;
        ApplyUiScale(TotalScale());
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_CLOSE:
        OnClose(hwnd);
        return 0;

    case window::tray::kMessage:
        if (LOWORD(lParam) == WM_LBUTTONUP) {
            ShowManager(g.hidden);
        } else if (LOWORD(lParam) == WM_RBUTTONUP) {
            bool toggle = false;
            window::tray::ShowMenu(hwnd, g.hidden, toggle);
            if (toggle) ShowManager(g.hidden);
        }
        return 0;

    case WM_HOTKEY:
        if (static_cast<int>(wParam) == window::hotkey::kId) ShowManager(g.hidden);
        return 0;

    case WM_TIMER:
        if (wParam == kTrayTimer) {
            KillTimer(hwnd, kTrayTimer);
            EnsureTrayIcon(hwnd);
            return 0;
        }
        if (wParam == kUpdateTimer) {
            update::Tick();
            const std::string notice = update::TakeNotice();
            if (!notice.empty()) {
                if (g.hidden) {   // nobody is looking at the window: say it from the notification area
                    wchar_t wide[256] = {};
                    MultiByteToWideChar(CP_UTF8, 0, notice.c_str(), -1, wide, 255);
                    window::tray::Balloon(L"A new version is available", wide);
                }   // with the window shown, the offer to update opens by itself (widgets::UpdateOffer)
            }
            return 0;
        }
        break;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;   // Alt alone does not open the (absent) system menu
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool OnScreen(const RECT& r) { return MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr; }

} // namespace

void GuiManager::RequestUserScale() { g.scaleDirty = true; }

void GuiManager::ApplyHotkey() {
    if (!g.hwnd) return;
    window::hotkey::Apply(g.hwnd);
    window::tray::SetTip(window::hotkey::Registered() ? window::hotkey::LabelFromConfig() : L"");
}

const char* GuiManager::HotkeyStatus() { return window::hotkey::Status(); }
void GuiManager::ToggleWindow() { ShowManager(g.hidden); }
bool GuiManager::WindowVisible() { return !g.hidden; }

void GuiManager::FailNextFramesForTest(int n) { g.failFramesForTest = n; }

void GuiManager::StartGuiThread(AddonManager* manager, std::function<void()> beforeAddons) {
    g.manager = manager;
    g.beforeAddons = std::move(beforeAddons);
    if (HANDLE thread = CreateThread(nullptr, 0, GuiThread, nullptr, 0, nullptr)) CloseHandle(thread);
}

DWORD WINAPI GuiManager::GuiThread(LPVOID /*lpParam*/) {
    // This thread's windows are per-monitor DPI aware whatever the process is (Lossless Scaling's manifest can keep Core::Init's process-wide
    // request from taking effect): sharp text at any display scaling, and window positions in physical pixels.
    using SetContextFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (const auto set = reinterpret_cast<SetContextFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")))
        set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto& cfg = ConfigManager::Instance();
    if (g.beforeAddons) g.beforeAddons();

    // Load addons in the GUI thread to avoid the loader lock. Settings > "Auto-load addons on startup": when off, addons wait for "Load now".
    if (g.manager) {
        if (cfg.GlobalGetOr<bool>(nullptr, "auto_load", true)) g.manager->LoadAddons();
        else LOG_INFO("GUI", "Auto-load is off; addons stay unloaded until loaded from the manager");
    }

    // Pairs with the DPI-awareness call in Core::Init. The window is created at the system scale and corrected below once we know which
    // monitor it actually landed on.
    const float systemScale = window::DisplayScale(nullptr);
    const window::Placement place = window::InitialPlacement(cfg.GlobalGet("ui", "window"), systemScale, OnScreen);

    const window::AppIcons icons = window::LoadAppIcons();

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandleW(nullptr), icons.big, nullptr, nullptr, nullptr, kWindowClass, icons.little };
    RegisterClassExW(&wc);
    const std::wstring title = WindowTitle();
    HWND hwnd = CreateWindowW(kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW, place.x, place.y, place.w, place.h, nullptr, nullptr, wc.hInstance, nullptr);
    window::ApplyDarkTitleBar(hwnd);

    g.displayScale = window::DisplayScale(hwnd);
    if (std::abs(g.displayScale - systemScale) > 0.01f) {
        SetWindowPos(hwnd, nullptr, place.x, place.y, static_cast<int>(place.w * g.displayScale / systemScale),
                     static_cast<int>(place.h * g.displayScale / systemScale), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (icons.big) {   // in case the class icons did not apply
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icons.big));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icons.little));
    }

    if (!g.device.Create(hwnd)) {
        LOG_ERROR("GUI", "Could not create the manager window's D3D11 device; the manager UI is unavailable");
        DestroyWindow(hwnd);
        UnregisterClassW(kWindowClass, wc.hInstance);
        return 1;
    }

    g.hwnd = hwnd;
    DragAcceptFiles(hwnd, TRUE);   // drop an addon on the window to install it
    g.trayIcon = icons.little;
    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g.trayTries = 0;
    EnsureTrayIcon(hwnd);
    GuiManager::ApplyHotkey();     // registers the hotkey and puts it in the tray tip
    update::Tick();                // the daily update check, if it is on and due
    SetTimer(hwnd, kUpdateTimer, 60 * 1000, nullptr);
    if (cfg.GlobalGetOr<bool>("ui", "open_on_start", true)) {
        ShowWindow(hwnd, place.maximized ? SW_SHOWMAXIMIZED : SW_SHOWDEFAULT);
        UpdateWindow(hwnd);
    } else {
        g.hidden = true;   // starts in the tray; the icon and the hotkey open it
        LOG_INFO("GUI", "Manager window starts hidden (Settings > Manager window)");
    }

    // Addon icons need the window's D3D11 device.
    SetIconDevice(g.device.Device());
    if (g.manager) g.manager->LoadAddonIcons();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    if (g.manager) g.manager->InitializeAddons(ImGui::GetCurrentContext());   // addons share this context

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;   // layout is ours; do not litter the Lossless Scaling folder with imgui.ini

    // Fonts can be overridden in the addon-manager config: "ui": { "font": "...ttf", "mono_font": "...ttf" }
    LoadUiFonts(cfg.GlobalGetOr<std::string>("ui", "font", ""), cfg.GlobalGetOr<std::string>("ui", "mono_font", ""));
    ApplyUiScale(TotalScale());

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g.device.Device(), g.device.Context());

    const float clearColor[4] = { 0.094f, 0.094f, 0.094f, 1.0f };
    LOG_INFO("GUI", "Addon Manager window ready");

    for (bool done = false; !done;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // Present does not wait for vsync while minimised, so block for the next message instead of spinning.
        if (g.minimized || g.hidden) { WaitMessage(); continue; }

        if (g.scaleDirty.exchange(false)) ApplyUiScale(TotalScale());   // between frames: the style is not in use
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        // Anything that throws while drawing or acting on a click (a settings file that cannot be written, a folder that vanished) must not
        // end Lossless Scaling: the half-drawn frame is unwound with ImGui's own error recovery, and the window carries on.
        ImGuiErrorRecoveryState recovery;
        ImGui::ErrorRecoveryStoreState(&recovery);
        const char* failure = nullptr;
        std::string why;
        try {
            if (g.failFramesForTest > 0) { --g.failFramesForTest; throw std::runtime_error("a test failure"); }
            window::RenderMainFrame(g.manager, g.bringAddonsForward);
        } catch (const std::exception& e) { failure = "exception"; why = e.what(); } catch (...) { failure = "exception"; why = "unknown"; }
        if (failure) {
            ImGui::ErrorRecoveryTryToRecoverState(&recovery);
            if (++g.frameFailures <= 3) {   // not every frame of a repeating problem
                LOG_ERROR("GUI", "Drawing the window failed (%s); the window carries on", why.c_str());
                widgets::ToastShow("Something went wrong in the manager window (see the Logs tab).", widgets::ToastType::Error, 6.0f);
            }
        }
        widgets::ToastRender();
        ImGui::Render();

        ID3D11RenderTargetView* target = g.device.BackBuffer();
        g.device.Context()->OMSetRenderTargets(1, &target, nullptr);
        g.device.Context()->ClearRenderTargetView(target, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        const HRESULT presentHr = g.device.SwapChain()->Present(1, 0);

        // This window sits next to a running game. Redrawing it at the display rate while nobody is looking wastes GPU and CPU the game
        // wants (and, when the window is covered, Present returns at once and the loop would spin). Drop to ~10 fps unless it has focus.
        const bool occluded = (presentHr == DXGI_STATUS_OCCLUDED);
        if (occluded || GetForegroundWindow() != hwnd) MsgWaitForMultipleObjects(0, nullptr, FALSE, occluded ? 250 : 100, QS_ALLINPUT);
    }

    KillTimer(hwnd, kUpdateTimer);
    KillTimer(hwnd, kTrayTimer);
    GpuStats::Instance().Shutdown();
    PersistPlacement();
    window::hotkey::Remove(hwnd);
    window::tray::Remove();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g.device.Destroy();
    DestroyWindow(hwnd);
    UnregisterClassW(kWindowClass, wc.hInstance);
    return 0;
}

} // namespace eam
