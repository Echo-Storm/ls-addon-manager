// Offline test of the built-in features. Nothing here is visible or takes focus: ReShade passthrough is tried on a plain popup window parked far
// off-screen and shown without activation, in this test's own process, and Windowed mode is tried by enumerating displays through DXGI and
// user32 the way Lossless Scaling does. Run it twice, because the Windowed hooks stay in the process once they are in:
//
//   eam_featurestest.exe          Windowed mode switched on at start-up: the virtual display appears, and can be switched off and on live
//   eam_featurestest.exe off      Windowed mode switched off at start-up: no virtual display, and switching it on says a restart is needed
#include "src/config/config_manager.h"
#include "src/features/features.h"
#include "src/features/reshade_passthrough.h"
#include "src/features/reshade_windows.h"
#include "src/features/windowed_mode.h"
#include <windows.h>
#include <dxgi1_6.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

namespace fs = std::filesystem;
using namespace eam;

static int g_fail = 0;
static void Check(const char* what, bool ok) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++g_fail; }

static int IndexOfFeature(const char* id) {
    for (int i = 0; i < features::Count(); ++i) if (!strcmp(features::At(i).id, id)) return i;
    return -1;
}

// ---- ReShade passthrough --------------------------------------------------------------------------------------------------------

static WNDPROC g_below = nullptr;
static std::atomic<int> g_topHits{ 0 };
static LRESULT CALLBACK TopProc(HWND h, UINT m, WPARAM w, LPARAM l) { ++g_topHits; return CallWindowProc(g_below, h, m, w, l); }

// The window lives on its own thread with a message loop, as in a real process: the watcher changes window styles, which sends messages to
// the owning thread, so that thread must be pumping.
struct WindowThread {
    std::thread t;
    std::atomic<HWND> hwnd{ nullptr };
    std::atomic<DWORD> tid{ 0 };
    const wchar_t* cls = L"STATIC";
    void Start() {
        t = std::thread([this] {
            tid = GetCurrentThreadId();
            HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls, L"features test", WS_POPUP, -32000, -32000, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            ShowWindow(h, SW_SHOWNOACTIVATE);
            hwnd = h;
            MSG m;
            while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
            DestroyWindow(h);
        });
        while (!hwnd.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    void Stop() {
        PostThreadMessageW(tid.load(), WM_QUIT, 0, 0);
        if (t.joinable()) t.join();
    }
};

static void ReShadeOnce(bool layerOnTop) {
    printf("== ReShade passthrough, %s\n", layerOnTop ? "another subclass on top of ours" : "plain window");
    const int idx = IndexOfFeature(features::At(0).id);
    WindowThread wt;
    wt.Start();
    HWND hwnd = wt.hwnd;
    Check("test window is visible (off-screen, not activated)", IsWindowVisible(hwnd) != 0);
    const WNDPROC original = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    // The manager's own window is in the same process but is no ReShade overlay: it must be left alone (restyling it from the watcher sends messages to
    // the manager's thread, which may be the very thread waiting for the watcher to stop)
    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LSAddonManagerClass";
    RegisterClassExW(&wc);
    WindowThread own;
    own.cls = L"LSAddonManagerClass";
    own.Start();
    const WNDPROC ownOriginal = (WNDPROC)GetWindowLongPtrW(own.hwnd, GWLP_WNDPROC);

    features::SetOn(idx, true);   // starts the watcher
    features::reshade::ForcePassthroughForTest(true);
    for (int i = 0; i < 100; ++i) {
        if ((WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC) != original) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const WNDPROC hooked = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    Check("passthrough on: the window was subclassed", hooked != original);
    Check("passthrough on: the status says so", features::Status(idx) == "Passthrough ON");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Check("passthrough on: the manager's own window is left alone", (WNDPROC)GetWindowLongPtrW(own.hwnd, GWLP_WNDPROC) == ownOriginal);

    if (layerOnTop) { g_below = hooked; g_topHits = 0; SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)TopProc); }

    const auto t0 = std::chrono::steady_clock::now();
    features::SetOn(idx, false);   // stops the watcher and puts the windows back
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("  switching off took %.0f ms\n", ms);
    Check("switching off returned promptly (watcher joined)", ms < 3000);
    Check("the status is empty again", features::Status(idx).empty());

    if (!layerOnTop) {
        Check("the window's original procedure is back", (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC) == original);
        Check("nothing is left hooked", !features::reshade::StillHooked());
    } else {
        Check("our procedure is still in the chain (it could not be removed), and is reported", features::reshade::StillHooked());
        const LRESULT r = SendMessageW(hwnd, WM_NULL, 0, 0);   // TopProc -> ours -> the original
        Check("messages still work through the chain", g_topHits.load() > 0 && r == 0);
    }
    wt.Stop();
    own.Stop();
    features::reshade::RestoreAll();
}

// ---- Windowed mode --------------------------------------------------------------------------------------------------------------

struct Outputs { int count = 0; bool hasVirtual = false; };

static Outputs EnumDxgi() {
    Outputs o;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return o;
    IDXGIAdapter1* a = nullptr;
    if (SUCCEEDED(f->EnumAdapters1(0, &a))) {
        for (UINT i = 0; i < 16; ++i) {
            IDXGIOutput* out = nullptr;
            if (a->EnumOutputs(i, &out) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC d{};
            out->GetDesc(&d);
            ++o.count;
            if (wcsstr(d.DeviceName, L"DISPLAY_VIRTUAL")) o.hasVirtual = true;
            out->Release();
        }
        a->Release();
    }
    f->Release();
    return o;
}

static BOOL CALLBACK CountMonitor(HMONITOR, HDC, LPRECT, LPARAM p) { ++*(int*)p; return TRUE; }
static int MonitorCount() { int n = 0; EnumDisplayMonitors(nullptr, nullptr, CountMonitor, (LPARAM)&n); return n; }

static void WindowedOnce(bool startOn) {
    printf("== Windowed mode, %s at start-up\n", startOn ? "switched on" : "switched off");
    const int idx = IndexOfFeature(features::At(1).id);
    const Outputs before = EnumDxgi();
    const int monitorsBefore = MonitorCount();
    printf("  before: %d DXGI output(s) on adapter 0, %d monitor(s)\n", before.count, monitorsBefore);
    Check("no virtual display before the feature starts", !before.hasVirtual);

    ConfigManager::Instance().SetAddonEnabled(features::At(idx).id, startOn);
    features::Start();   // what Lossless Scaling start-up does: starts what is switched on
    std::this_thread::sleep_for(std::chrono::milliseconds(800));   // the hooks go in on their own thread

    const Outputs now = EnumDxgi();
    const int monitorsNow = MonitorCount();
    printf("  after start-up: %d DXGI output(s), %d monitor(s)\n", now.count, monitorsNow);
    if (startOn) {
        Check("on: one extra DXGI output, the virtual display", now.count == before.count + 1 && now.hasVirtual);
        Check("on: one extra monitor in EnumDisplayMonitors", monitorsNow == monitorsBefore + 1);
        Check("on: it does not ask for a restart", !features::NeedsRestart(idx));

        features::SetOn(idx, false);   // live: the hooks stay in and do nothing
        const Outputs off = EnumDxgi();
        Check("switched off while running: the virtual display goes away at once", off.count == before.count && !off.hasVirtual && MonitorCount() == monitorsBefore);
        features::SetOn(idx, true);
        const Outputs again = EnumDxgi();
        Check("switched on again while running: it comes back at once", again.count == before.count + 1 && again.hasVirtual && !features::NeedsRestart(idx));
    } else {
        Check("off: the virtual display is not offered", now.count == before.count && !now.hasVirtual);
        Check("off: no extra monitor in EnumDisplayMonitors", monitorsNow == monitorsBefore);
        features::SetOn(idx, true);
        Check("switching it on later says a restart is needed", features::NeedsRestart(idx));
        Check("...and still shows no virtual display", !EnumDxgi().hasVirtual);
        features::SetOn(idx, false);
        Check("switching it off again clears that", !features::NeedsRestart(idx));
    }

    features::Stop();
    const Outputs after = EnumDxgi();   // must still work: the hooks are out, and wrapped objects made earlier are inert
    Check("DXGI still works after the feature is stopped", after.count == before.count);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const bool startOn = !(argc > 1 && !strcmp(argv[1], "off"));

    const fs::path dir = fs::temp_directory_path() / ("eam_featurestest_" + std::to_string(GetCurrentProcessId()));
    {   // a fresh folder every run: process ids are reused, and an earlier run (the abrupt one) must not leave a switched-on feature behind
        std::error_code cleanup;
        fs::remove_all(dir, cleanup);
    }
    fs::create_directories(dir);
    ConfigManager::Instance().Load((dir / "config.json").wstring());

    if (argc > 1 && !strcmp(argv[1], "abrupt")) {
        // The process ends with a feature's watcher thread running and features::Stop never called, which is what can happen when Lossless
        // Scaling exits. A std::thread still joinable when this program's statics are destroyed calls std::terminate: the exit code would be a crash.
        features::SetOn(0, true);   // ReShade passthrough: starts its watcher thread
        Sleep(300);
        printf("abrupt exit with the ReShade watcher running\n");
        {
            std::error_code cleanup;
            fs::remove_all(dir, cleanup);   // its settings are saved by now; leave no folder behind
        }
        exit(0);   // runs the static destructors, as a DLL's are run when the process ends
    }

    printf("== the list of features\n");
    Check("four features are built in", features::Count() == 4 && IndexOfFeature("ReShadePassthrough") == 0 && IndexOfFeature("WindowedMode") == 1 && IndexOfFeature("Recorder") == 2 && IndexOfFeature("GpuLimit") == 3);
    {   // the recorder feature: switched on it says so (no frames yet), off it says nothing and holds nothing
        const int rec = IndexOfFeature("Recorder");
        features::SetOn(rec, true);
        Check("the Recording feature switched on says it waits for frames", features::IsOn(rec) && features::Status(rec).find("waiting for frames") != std::string::npos);
        features::SetOn(rec, false);
        Check("...and off says nothing", !features::IsOn(rec) && features::Status(rec).empty());
        const int gpu = IndexOfFeature("GpuLimit");
        features::SetOn(gpu, true);
        Check("the GPU limit switched on says its limit (95 % by default)", features::IsOn(gpu) && features::Status(gpu).find("Limit 95 %") == 0);
        features::SetOn(gpu, false);
        Check("...and off says nothing", !features::IsOn(gpu) && features::Status(gpu).empty());
    }
    Check("the retired standalone addons are recognised by folder name", features::IsRetiredAddonId("LSP-ReShade") && features::IsRetiredAddonId("LSP-Windowed") && !features::IsRetiredAddonId("DLSS5NR01"));
    Check("a feature is off until switched on", !features::IsOn(0) && !features::IsOn(1));
    features::SetOn(0, true);
    Check("switching one on is saved under the feature's id", ConfigManager::Instance().IsAddonEnabled("ReShadePassthrough", false));
    features::SetOn(0, false);
    {   // settings saved up to 0.7.4 sit under the old ids
        ConfigManager& cfg = ConfigManager::Instance();
        cfg.Set("LSP-Windowed", "test_marker", "moved");
        cfg.Set("LSP-ReShade", "test_marker", "old");
        features::MoveOldSettings();
        Check("settings saved under a feature's old id move to its id", cfg.Get("WindowedMode", "test_marker", "") == "moved" && cfg.Get("LSP-Windowed", "test_marker", "") == "");
        Check("...but never over settings the new id already has", cfg.Get("ReShadePassthrough", "test_marker", "") == "" && cfg.Get("LSP-ReShade", "test_marker", "") == "old");
    }

    if (startOn) { ReShadeOnce(false); ReShadeOnce(true); }
    WindowedOnce(startOn);

    std::error_code ec;
    fs::remove_all(dir, ec);
    printf("\n%s\n", g_fail ? "FEATURES TEST FAILED" : "FEATURES TEST PASSED");
    return g_fail ? 1 : 0;
}
