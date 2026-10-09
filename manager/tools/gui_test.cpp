// Smoke test of the manager window: the real GUI thread, window, hotkey and tray code, driven by window messages. It starts the window hidden
// and only shows it for a moment while testing the toggle, then checks: the title, that the window exists and starts hidden, that a toggle shows and hides it, that closing
// hides instead of ending the thread and saves the placement, that the hotkey message toggles it, the minimum size, where a saved placement puts
// the window, and that the thread ends cleanly and takes the window with it. Works on a throw-away config in %TEMP%; touches no game and no Lossless
// Scaling install. Needs a GPU (the window has its own D3D11 device).
//   eam_guitest.exe            fresh config: default placement
//   eam_guitest.exe place      a saved placement in the config: the window must open there
//   eam_guitest.exe scaled     the same with the interface size at 150 %: closing must save the size it was loaded with
// The first run also checks the pure parts (placement, hotkey text, scale limits, status text, tray tip) when those modules exist.
#include "src/addon/addon_manager.h"
#include "src/config/config_manager.h"
#include "src/gui/gui_manager.h"
#include "src/host/host_impl.h"
#include "src/log/logger.h"
#include "eam/version.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#if __has_include("src/gui/window/placement.h")
#define HAVE_WINDOW_MODULES 1
#include "src/gui/window/dpi.h"
#include "src/gui/window/hotkey.h"
#include "src/gui/window/placement.h"
#include "src/gui/window/status_text.h"
#include "src/gui/window/tray.h"
#endif

namespace fs = std::filesystem;
using namespace eam;

static int g_failed = 0;
static void Check(const char* what, bool ok, const std::string& detail = "") {
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  (", detail.empty() ? "" : (detail + ")").c_str());
    if (!ok) ++g_failed;
}

static const wchar_t* kClass = L"LSAddonManagerClass";
static constexpr UINT kHotkeyMsgId = 0x4C50;   // the id the window registers its hotkey under

static bool TrayAdded() { return eam::window::tray::Added(); }
static bool WaitFor(bool (*cond)(), int ms) {
    for (int t = 0; t < ms; t += 20) { if (cond()) return true; Sleep(20); }
    return cond();
}
// Only a window of THIS process: a real manager window (Lossless Scaling running with the manager) has the same class name, and this test sends
// close and destroy messages, which must never reach it.
static HWND Find() {
    for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, kClass, nullptr)) != nullptr;) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) return h;
    }
    return nullptr;
}
static bool WindowThere() { return Find() != nullptr; }
static bool WindowGone() { return Find() == nullptr; }
static bool Hidden() { return !GuiManager::WindowVisible(); }
static bool Shown() { return GuiManager::WindowVisible(); }
static bool WinVisibleNow() { HWND h = Find(); return h && IsWindowVisible(h); }
static bool WinHiddenNow() { HWND h = Find(); return h && !IsWindowVisible(h); }

#ifdef HAVE_WINDOW_MODULES
static void PureChecks() {
    using namespace eam::window;
    const OnScreenFn yes = [](const RECT&) { return true; };
    const OnScreenFn no = [](const RECT&) { return false; };

    Placement d = InitialPlacement(nlohmann::json(), 1.0f, yes);
    Check("placement: nothing saved gives the default size at (100, 100)", d.w == 1100 && d.h == 720 && d.x == 100 && d.y == 100 && !d.maximized);
    d = InitialPlacement(nlohmann::json(), 1.5f, yes);
    Check("placement: the default size follows the display scale", d.w == 1650 && d.h == 1080);
    const nlohmann::json ok = { {"x", 210}, {"y", 130}, {"w", 900}, {"h", 600}, {"maximized", true} };
    d = InitialPlacement(ok, 1.25f, yes);
    Check("placement: a saved one is used, its size scaled", d.x == 210 && d.y == 130 && d.w == 1125 && d.h == 750 && d.maximized);
    d = InitialPlacement({ {"x", 5}, {"y", 6}, {"w", 700}, {"h", 600} }, 1.0f, yes);
    Check("placement: a saved width under the minimum is ignored", d.w == 1100 && d.h == 720 && d.x == 100 && d.y == 100);
    d = InitialPlacement({ {"x", 5}, {"y", 6}, {"w", 900}, {"h", 470} }, 1.0f, yes);
    Check("placement: a saved height under the minimum is ignored", d.w == 1100 && d.h == 720);
    d = InitialPlacement(ok, 1.0f, no);
    Check("placement: a position on a monitor that is gone falls back to (100, 100), keeping the size", d.x == 100 && d.y == 100 && d.w == 900 && d.h == 600);
    d = InitialPlacement(nlohmann::json({ {"w", 900}, {"h", 600} }), 1.0f, yes);
    Check("placement: a saved size without a position uses (100, 100)", d.x == 100 && d.y == 100 && d.w == 900);
    d = InitialPlacement(nlohmann::json("nonsense"), 1.0f, yes);
    Check("placement: a setting that is not an object is ignored", d.w == 1100);
    d = InitialPlacement(nlohmann::json({ {"x", "left"}, {"y", nullptr}, {"w", "900"}, {"h", 600}, {"maximized", "yes"} }), 1.0f, yes);
    Check("placement: values of the wrong type (a hand-edited config.json) are the defaults, not an exception", d.w == 1100 && d.h == 720 && !d.maximized);
    d = InitialPlacement(nlohmann::json({ {"x", "left"}, {"y", nullptr}, {"w", 900}, {"h", 600}, {"maximized", 1} }), 1.0f, yes);
    Check("placement: a good size with a position of the wrong type keeps the size and the default position", d.w == 900 && d.h == 600 && d.x == 100 && d.y == 100 && !d.maximized);
    bool asked = false;
    const OnScreenFn spy = [&](const RECT& r) { asked = (r.left == 210 && r.top == 130 && r.right == 1110 && r.bottom == 730); return true; };
    InitialPlacement(ok, 1.0f, spy);
    Check("placement: the monitor test gets the window's rectangle", asked);

    Check("hotkey: F12 is the default and F1..F12 pass through", hotkey::NormalizeVk(VK_F12) == VK_F12 && hotkey::NormalizeVk(VK_F1) == VK_F1 && hotkey::NormalizeVk(VK_F7) == VK_F7);
    Check("hotkey: anything else falls back to F12", hotkey::NormalizeVk(0) == VK_F12 && hotkey::NormalizeVk(VK_F13) == VK_F12 && hotkey::NormalizeVk(-5) == VK_F12);
    Check("hotkey: the label names the combination", hotkey::Label(VK_F12) == L"Ctrl+Shift+F12" && hotkey::Label(VK_F1) == L"Ctrl+Shift+F1" && hotkey::Label(999) == L"Ctrl+Shift+F12");

    Check("scale: the interface size is kept between 75 % and 200 %", ClampScalePercent(50) == 75 && ClampScalePercent(75) == 75 && ClampScalePercent(130) == 130 &&
          ClampScalePercent(200) == 200 && ClampScalePercent(400) == 200);


    const std::string base = StatusCounts(3, 1);
    Check("status: counts", base.find("   |   3 addons, 1 on") != std::string::npos && base.rfind(EAM_PRODUCT_SHORT " " EAM_VERSION_STRING, 0) == 0, base);
    Check("status: one addon is singular", StatusCounts(1, 0).find("   |   1 addon, 0 on") != std::string::npos);
    Check("status: none is plural", StatusCounts(0, 0).find("   |   0 addons, 0 on") != std::string::npos);
    Check("status: a live status is added after the counts", WithLiveStatus("A", "Neural Rendering", "12 ms") == "A   |   Neural Rendering: 12 ms" && WithLiveStatus("A", "x", "") == "A");

    Check("status: an update is added when there is one", WithUpdate("A", "0.5.0") == "A   |   Update available: 0.5.0" && WithUpdate("A", "") == "A");
    const std::wstring tip = tray::TipText(L"");
    Check("tray tip: the product name and what a click does", tip == std::wstring(EAM_PRODUCT_SHORT_W) + L": click to open or close");
    Check("tray tip: the hotkey in brackets when there is one", tray::TipText(L"Ctrl+Shift+F12") == tip + L" (Ctrl+Shift+F12)");
}
#endif

int wmain(int argc, wchar_t** argv) {
    // per-monitor DPI aware, as the manager's window thread is: every rectangle in physical pixels
    using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (const auto set = reinterpret_cast<SetContextFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))) set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring mode = argc > 1 ? argv[1] : L"";
    const bool place = mode == L"place" || mode == L"scaled";
    const bool scaled = mode == L"scaled";
#ifdef HAVE_WINDOW_MODULES
    if (mode.empty()) PureChecks();
#endif
    const fs::path dir = fs::temp_directory_path() / (L"eam_guitest_" + (mode.empty() ? std::wstring(L"plain") : mode));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "addons");

    Logger::Instance().Init((dir / "test.log").wstring());
    ConfigManager& cfg = ConfigManager::Instance();
    cfg.Load((dir / "addons" / "config.json").wstring());
    HostImpl host;
    AddonManager mgr(&host, (dir / "addons").wstring());   // reads the config file again: set our values after it
    cfg.GlobalSet("ui", "open_on_start", false);          // no window appears
    cfg.GlobalSet("ui", "hotkey_vk", (int)VK_F9);         // an F-key the real manager (F12) is unlikely to be holding
    cfg.GlobalSet("ui", "hide_hint_shown", true);         // no tray balloon from the test
    cfg.GlobalSet(nullptr, "auto_load", false);
    cfg.GlobalSet("updates", "check", false);   // the update check is on by default: this test must not touch the internet
    if (scaled) cfg.GlobalSet("ui", "scale_percent", 150);
    if (place) cfg.GlobalSet("ui", "window", nlohmann::json{ {"x", 210}, {"y", 130}, {"w", 900}, {"h", 600}, {"maximized", false} });
    GuiManager::StartGuiThread(&mgr);

    if (!WaitFor(WindowThere, 20000)) { printf("FAIL  the manager window was not created within 20 s (is there a GPU?)\n"); return 2; }
    HWND hwnd = Find();
    Check("the window exists", hwnd != nullptr);

    wchar_t title[128] = {};
    GetWindowTextW(hwnd, title, 128);
    std::wstring want = std::wstring(EAM_PRODUCT_NAME_W) + L" v";
    for (const char* c = EAM_VERSION_STRING; *c; ++c) want += (wchar_t)*c;
    Check("the title is the product name and version", want == title);

    // the window exists early; the thread marks it hidden (open on start off) a little later, once its D3D device and the tray are set up
    Check("it starts hidden when 'open on start' is off", WaitFor(Hidden, 20000) && WinHiddenNow());
    Sleep(300);   // the render loop and the ImGui set-up follow
    const std::string hk = GuiManager::HotkeyStatus();
    const bool hkOk = hk == "registered";
    Check("the hotkey is registered (or another program holds it)", hkOk || hk.rfind("could not register", 0) == 0, hk);

    // size and placement
    const UINT dpi = GetDpiForSystem();
    const float sc = dpi / 96.0f;
    RECT r; GetWindowRect(hwnd, &r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (place) {
        Check("a saved placement is used: position", r.left == 210 && r.top == 130, std::to_string(r.left) + "," + std::to_string(r.top));
        Check("a saved placement is used: size scales with the display", w == (int)(900 * sc) && h == (int)(600 * sc), std::to_string(w) + "x" + std::to_string(h));
    } else {
        Check("the default size is 1100x720 at the display scale", w == (int)(1100 * sc) && h == (int)(720 * sc), std::to_string(w) + "x" + std::to_string(h));
    }
    MINMAXINFO mmi = {};
    SendMessageW(hwnd, WM_GETMINMAXINFO, 0, (LPARAM)&mmi);
    const float minScale = sc * (scaled ? 1.5f : 1.0f);   // the minimum follows the whole interface scale, the placement only the display's
    Check("the minimum size is 760x480 at the interface scale", mmi.ptMinTrackSize.x == (LONG)(760.0f * minScale) && mmi.ptMinTrackSize.y == (LONG)(480.0f * minScale),
          std::to_string(mmi.ptMinTrackSize.x) + "x" + std::to_string(mmi.ptMinTrackSize.y));

    // show and hide
    GuiManager::ToggleWindow();
    Check("toggle shows the window", Shown() && WaitFor(WinVisibleNow, 2000));
    GuiManager::ToggleWindow();
    Check("toggle hides it again", Hidden() && WaitFor(WinHiddenNow, 2000));

    // the hotkey message
    SendMessageW(hwnd, WM_HOTKEY, kHotkeyMsgId, 0);
    Check("the hotkey message shows the window", Shown() && WaitFor(WinVisibleNow, 2000));
    SendMessageW(hwnd, WM_HOTKEY, kHotkeyMsgId, 0);
    Check("...and hides it", Hidden() && WaitFor(WinHiddenNow, 2000));
    SendMessageW(hwnd, WM_HOTKEY, kHotkeyMsgId + 1, 0);
    Check("another hotkey id is ignored", Hidden());

    // closing hides, keeps the thread, and saves the placement
    GuiManager::ToggleWindow();
    WaitFor(WinVisibleNow, 2000);
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    Check("closing hides the window instead of destroying it", Hidden() && WinHiddenNow() && WindowThere());
    const nlohmann::json saved = cfg.GlobalGet("ui", "window");
    const int sw = saved.is_object() ? saved.value("w", 0) : 0, sh = saved.is_object() ? saved.value("h", 0) : 0;
    Check("...and saves the placement in logical pixels", saved.is_object() && sw >= 760 && sh >= 480 && !saved.value("maximized", true),
          saved.is_object() ? std::to_string(sw) + "x" + std::to_string(sh) : "nothing saved");
    if (place) Check("...the saved size is what was loaded", sw == 900 && sh == 600, std::to_string(sw) + "x" + std::to_string(sh));
    SendMessageW(hwnd, WM_HOTKEY, kHotkeyMsgId, 0);
    Check("it comes back from the tray after a close", Shown() && WaitFor(WinVisibleNow, 2000));

    // let it draw a few frames while visible and then drop to hidden again; nothing should fall over
    Sleep(500);
    Check("still there after drawing frames", WindowThere());

    // drawing that throws (a settings file that cannot be written, a folder that vanished) must not end the process: the frame is unwound and the window carries on
    GuiManager::FailNextFramesForTest(5);
    Sleep(600);
    bool logged = false;
    for (const auto& e : Logger::Instance().GetEntries(LogLevel::Error)) if (e.message.find("Drawing the window failed") != std::string::npos) logged = true;
    Check("frames that throw while drawing are survived, and said so in the log", WindowThere() && WinVisibleNow() && logged);
    Sleep(300);
    Check("...and the window draws normally afterwards", WindowThere() && WinVisibleNow());
    SendMessageW(hwnd, WM_SIZE, SIZE_MINIMIZED, 0);
    Sleep(100);
    SendMessageW(hwnd, WM_HOTKEY, kHotkeyMsgId, 0);
    Check("hiding while minimised works", Hidden());

    // the tray-created message (Explorer restarted) must not crash the window
    const UINT wmTaskbar = RegisterWindowMessageW(L"TaskbarCreated");
    SendMessageW(hwnd, wmTaskbar, 0, 0);
    Check("a restarted taskbar is handled", WindowThere());
    Check("...and the icon is back afterwards", WaitFor(TrayAdded, 3000));

    // Explorer can be slow to take the icon after a restart (a live log once showed one failed re-add lose the icon for the whole session): the window keeps trying.
    // The next two attempts to add the icon are made to fail; the icon must come back by itself.
    eam::window::tray::FailNextAddsForTest(2);
    SendMessageW(hwnd, wmTaskbar, 0, 0);
    Check("a failed re-add leaves no icon for now", !TrayAdded());
    Check("the window retries by itself and the icon comes back", WaitFor(TrayAdded, 15000));
    Check("the window is still fine after the retries", WindowThere());

    // teardown: destroying the window ends the loop; the window class and the window go away
    SendMessageW(hwnd, WM_DESTROY, 0, 0);
    Check("the window and its thread wind down", WaitFor(WindowGone, 5000));

    printf("\n%s (%d failed)\n", g_failed ? "GUI TEST FAILED" : "GUI TEST PASSED", g_failed);
    fflush(stdout);
    return g_failed ? 1 : 0;
}
