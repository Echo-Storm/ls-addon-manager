#include "placement.h"

namespace eam {
namespace window {

Placement InitialPlacement(const nlohmann::json& saved, float displayScale, const OnScreenFn& onScreen) {
    Placement p;
    p.w = static_cast<int>(kDefaultLogicalW * displayScale);
    p.h = static_cast<int>(kDefaultLogicalH * displayScale);

    if (saved.is_object()) {
        // (a value of the wrong type, from a hand-edited or imported config.json, is the default: json's value() throws on it, and this runs before any guard)
        const auto intOf = [&](const char* key, int fallback) {
            const auto it = saved.find(key);
            return it != saved.end() && it->is_number_integer() ? static_cast<int>(it->get<long long>()) : fallback;
        };
        const int lw = intOf("w", 0), lh = intOf("h", 0);
        if (lw >= kMinLogicalW && lh >= kMinLogicalH) {
            p.w = static_cast<int>(lw * displayScale);
            p.h = static_cast<int>(lh * displayScale);
            p.x = intOf("x", p.x);
            p.y = intOf("y", p.y);
            const auto maxi = saved.find("maximized");
            p.maximized = maxi != saved.end() && maxi->is_boolean() && maxi->get<bool>();
        }
    }

    const RECT want = { p.x, p.y, p.x + p.w, p.y + p.h };
    if (onScreen && !onScreen(want)) { p.x = 100; p.y = 100; }
    return p;
}

SavedPlacement CapturePlacement(HWND hwnd, float displayScale) {
    SavedPlacement s;
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (!GetWindowPlacement(hwnd, &wp) || displayScale <= 0.0f) return s;
    const RECT& r = wp.rcNormalPosition;   // the restored rectangle, so a maximised window keeps the size it returns to
    s.x = r.left;
    s.y = r.top;
    s.w = static_cast<int>((r.right - r.left) / displayScale + 0.5f);
    s.h = static_cast<int>((r.bottom - r.top) / displayScale + 0.5f);
    s.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    s.valid = true;
    return s;
}

nlohmann::json ToJson(const SavedPlacement& p) {
    return nlohmann::json{ {"x", p.x}, {"y", p.y}, {"w", p.w}, {"h", p.h}, {"maximized", p.maximized} };
}

} // namespace window
} // namespace eam
