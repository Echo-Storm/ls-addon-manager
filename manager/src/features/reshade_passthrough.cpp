#include "reshade_passthrough.h"
#include "reshade_input.h"
#include "reshade_windows.h"
#include "../config/config_manager.h"
#include "../log/logger.h"
#include "imgui.h"
#include "eam/widgets.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <windows.h>

namespace eam {
namespace features {
namespace reshade {

namespace {

// ---- settings, kept as atomics because the watcher thread reads them while the window changes them
std::atomic<int> g_hotkeyVk{ VK_HOME };
std::atomic<bool> g_hotkeyCtrl{ false };
std::atomic<bool> g_hotkeyAlt{ false };
std::atomic<bool> g_hotkeyShift{ false };
std::atomic<bool> g_autoClick{ true };

// ---- the watcher
// If the process ends with the watcher still running (features::Stop was not called), the holder lets go of it instead of destroying a joinable
// std::thread, which would call std::terminate and crash Lossless Scaling at exit.
struct WatcherThread {
    std::thread t;
    ~WatcherThread() { if (t.joinable()) t.detach(); }
    bool joinable() const { return t.joinable(); }
    void join() { t.join(); }
    WatcherThread& operator=(std::thread&& other) { t = std::move(other); return *this; }
} g_watcher;
std::atomic<bool> g_running{ false };
std::atomic<bool> g_simulating{ false };      // an auto click and repress is in progress: ignore the hotkey meanwhile
std::atomic<int> g_clickThreads{ 0 };         // auto click threads still running
std::atomic<bool> g_ignoreFocusForTest{ false };

ConfigManager& Cfg() { return ConfigManager::Instance(); }

void LoadSettings() {
    g_hotkeyVk = std::atoi(Cfg().Get(kId, "hotkey_vk", "36").c_str());   // 36 = VK_HOME
    g_hotkeyCtrl = Cfg().Get(kId, "hotkey_ctrl", "0") == "1";
    g_hotkeyAlt = Cfg().Get(kId, "hotkey_alt", "0") == "1";
    g_hotkeyShift = Cfg().Get(kId, "hotkey_shift", "0") == "1";
    g_autoClick = Cfg().Get(kId, "auto_click_repress", "1") == "1";
}

void SaveSettings() {
    Cfg().Set(kId, "hotkey_vk", std::to_string(g_hotkeyVk.load()));
    Cfg().Set(kId, "hotkey_ctrl", g_hotkeyCtrl ? "1" : "0");
    Cfg().Set(kId, "hotkey_alt", g_hotkeyAlt ? "1" : "0");
    Cfg().Set(kId, "hotkey_shift", g_hotkeyShift ? "1" : "0");
    Cfg().Set(kId, "auto_click_repress", g_autoClick ? "1" : "0");
    Cfg().Save();
}

// ---- which windows count
bool BelongsToUs(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

// Lossless Scaling's own window (the first visible one it owns).
HWND FindLosslessWindow() {
    HWND found = nullptr;
    EnumWindows([](HWND hwnd, LPARAM out) -> BOOL {
        if (BelongsToUs(hwnd) && IsWindowVisible(hwnd)) { *(HWND*)out = hwnd; return FALSE; }
        return TRUE;
    }, (LPARAM)&found);
    return found;
}

// The window under Lossless Scaling's that is the game (the next visible window of another process, ignoring the desktop and taskbar).
HWND FindGameWindow(HWND lossless) {
    if (!lossless) return nullptr;
    for (HWND h = GetWindow(lossless, GW_HWNDNEXT); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsWindowVisible(h) || BelongsToUs(h)) continue;
        char cls[256];
        GetClassNameA(h, cls, sizeof cls);
        if (strcmp(cls, "Progman") && strcmp(cls, "Shell_TrayWnd") && strcmp(cls, "WorkerW")) return h;
    }
    return nullptr;
}

// Passthrough only makes sense while the game or Lossless Scaling has the focus.
bool FocusIsOurs() {
    if (g_ignoreFocusForTest) return true;
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    if (BelongsToUs(foreground)) return true;
    const HWND lossless = FindLosslessWindow();
    return lossless && foreground == FindGameWindow(lossless);
}

bool HotkeyDown() {
    const int vk = g_hotkeyVk;
    if (vk == 0) return false;
    auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    return down(vk) && down(VK_CONTROL) == g_hotkeyCtrl.load() && down(VK_MENU) == g_hotkeyAlt.load() && down(VK_SHIFT) == g_hotkeyShift.load();
}

// ReShade needs a click to take the focus, and the hotkey pressed again to open its menu; turning off needs the click to give it back.
void ClickAndRepress(bool turningOn) {
    g_simulating = true;
    struct Finished { ~Finished() { g_simulating = false; --g_clickThreads; } } finished;   // even if a sleep is cut short

    const int vk = g_hotkeyVk;
    const bool ctrl = g_hotkeyCtrl, alt = g_hotkeyAlt, shift = g_hotkeyShift;
    auto wait = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };

    if (turningOn) {
        wait(250);
        Click();
        wait(200);
        PressCombo(vk, ctrl, alt, shift);
    } else {
        wait(450);
        Click();
    }
    wait(200);
}

// The manager's own window is in this process too, but it is no ReShade overlay. Restyling it from here would send messages to the manager's
// thread, which may be the very thread waiting in Stop() for this watcher to end: leave it (and its children) alone.
bool IsManagerWindow(HWND hwnd) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    return wcscmp(cls, L"LSAddonManagerClass") == 0;
}

void ProcessOwnWindows() {
    EnumWindows([](HWND top, LPARAM) -> BOOL {
        if (BelongsToUs(top) && IsWindowVisible(top) && !IsManagerWindow(top)) {
            ProcessWindow(top);
            EnumChildWindows(top, [](HWND child, LPARAM) -> BOOL { ProcessWindow(child); return TRUE; }, 0);
        }
        return TRUE;
    }, 0);
}

void Watch() {
    LOG_INFO("ReShade", "Watcher started");
    bool wasDown = false;
    int sinceCleanup = 0;

    while (g_running) {
        if (g_passthroughOn && !FocusIsOurs()) {
            g_passthroughOn = false;
            LOG_INFO("ReShade", "Passthrough disabled: focus lost");
            RestoreAll();
        }

        const bool down = HotkeyDown();
        if (down && !wasDown && !g_simulating && FocusIsOurs()) {
            const bool turningOn = !g_passthroughOn.load();
            g_passthroughOn = turningOn;
            LOG_INFO("ReShade", "Passthrough %s", turningOn ? "ON" : "OFF");
            if (!turningOn) RestoreAll();
            if (g_autoClick && g_running) {
                ++g_clickThreads;
                std::thread([turningOn] { ClickAndRepress(turningOn); }).detach();
            }
        }
        wasDown = down;

        if (g_passthroughOn) {
            ProcessOwnWindows();
            if (++sinceCleanup >= 100) { CleanupDeadWindows(); sinceCleanup = 0; }
            ClipCursor(nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    LOG_INFO("ReShade", "Watcher stopped");
}

const char* KeyName(int vk) {
    static char text[32];
    if (vk == 0) return "None";
    if (vk >= VK_F1 && vk <= VK_F12) { sprintf_s(text, "F%d", vk - VK_F1 + 1); return text; }
    switch (vk) {
        case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_PRIOR: return "Page Up";
        case VK_NEXT: return "Page Down";
    }
    if (vk >= 'A' && vk <= 'Z') { text[0] = (char)vk; text[1] = 0; return text; }
    sprintf_s(text, "Key %d", vk);
    return text;
}

} // namespace

void Start() {
    if (g_running) return;
    LoadSettings();
    if (g_watcher.joinable()) g_watcher.join();   // a previous run still winding down (joined while g_running is false: its loop ends; with it already true again the join would never return)
    g_running = true;
    g_watcher = std::thread(Watch);
    LOG_INFO("ReShade", "ReShade input passthrough is on");
}

void Stop() {
    if (!g_running && !g_watcher.joinable()) return;
    g_running = false;
    if (g_watcher.joinable()) g_watcher.join();
    // an auto click in flight finishes its sleeps (well under two seconds) before this returns
    for (int i = 0; i < 200 && g_clickThreads.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    g_passthroughOn = false;
    RestoreAll();
    if (StillHooked()) LOG_WARN("ReShade", "A window still uses the passthrough procedure (something subclassed it after us); it stays in that window's chain");
    LOG_INFO("ReShade", "ReShade input passthrough is off");
}

std::string Status() { return g_passthroughOn ? "Passthrough ON" : std::string(); }

void ForcePassthroughForTest(bool on) {
    g_ignoreFocusForTest = on;
    g_passthroughOn = on;
}

void RenderOptions() {
    bool changed = false;

    bool autoClick = g_autoClick;
    if (ImGui::Checkbox("Auto click and repress", &autoClick)) { g_autoClick = autoClick; changed = true; }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("When you turn passthrough on or off, this clicks the mouse once (wherever the pointer is) and, when turning it on,\n"
                          "presses the hotkey again. ReShade needs that click to take focus, and the second press to open its menu.\n"
                          "Turn it off if you would rather do that yourself: the click lands on whatever is under the pointer.");

    bool ctrl = g_hotkeyCtrl, alt = g_hotkeyAlt, shift = g_hotkeyShift;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Hotkey");
    ImGui::SameLine();
    if (ImGui::Checkbox("Ctrl", &ctrl)) { g_hotkeyCtrl = ctrl; changed = true; }
    ImGui::SameLine();
    if (ImGui::Checkbox("Alt", &alt)) { g_hotkeyAlt = alt; changed = true; }
    ImGui::SameLine();
    if (ImGui::Checkbox("Shift", &shift)) { g_hotkeyShift = shift; changed = true; }
    ImGui::SameLine();

    static const int kKeys[] = { 0, VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
                                 VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT,
                                 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
                                 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z' };
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    const int current = g_hotkeyVk;
    if (ImGui::BeginCombo("##reshade_key", KeyName(current))) {
        for (int vk : kKeys)
            if (ImGui::Selectable(KeyName(vk), current == vk)) { g_hotkeyVk = vk; changed = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The key that turns passthrough on and off, with the modifiers ticked. Press it while the game has the focus.");

    if (changed) SaveSettings();
}

} // namespace reshade
} // namespace features
} // namespace eam
