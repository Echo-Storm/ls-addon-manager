#pragma once
// Vector icons for LS Addon Manager and its addons: SVG path data drawn straight into ImGui's draw list, so they are crisp at any
// display scale, take the colour of the surrounding UI, need no image files or textures, and work inside an addon (which has no
// access to the host's textures).
//
// A tiny SVG path renderer: M m L l H h V v C c S s Q q T t A a Z z, absolute and relative, implicit repeats, compact numbers
// ("1.49-1.46", ".5.5") and compact arc flags. Curves and arcs are flattened to polylines; closed shapes can be filled (concave
// fills work), everything can be stroked with round caps. To use an icon of your own, paste its `d="..."` string.
//
// The bundled shapes are drawn on a 24 x 24 grid in the manner of the Lucide icon set (ISC licence), which is a good source of
// more: https://lucide.dev
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace eam::ui {
namespace svg {

struct SubPath { std::vector<ImVec2> pts; bool closed = false; };

namespace detail {

struct Reader {
    const char* p;
    void SkipSep() { while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') ++p; }
    bool Number(float& out) {
        SkipSep();
        const char c = *p;
        if (!((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.')) return false;
        char* e = nullptr;
        out = (float)strtod(p, &e);
        if (e == p) return false;
        p = e;
        return true;
    }
    bool Flag(bool& out) { SkipSep(); if (*p == '0' || *p == '1') { out = *p == '1'; ++p; return true; } return false; }
    bool Command(char& c) { SkipSep(); if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) { c = *p++; return true; } return false; }
    bool AtEnd() { SkipSep(); return *p == 0; }
};

inline void Cubic(std::vector<ImVec2>& o, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d) {
    const int n = 14;
    for (int i = 1; i <= n; ++i) {
        const float t = (float)i / n, u = 1 - t;
        o.push_back(ImVec2(u * u * u * a.x + 3 * u * u * t * b.x + 3 * u * t * t * c.x + t * t * t * d.x,
                           u * u * u * a.y + 3 * u * u * t * b.y + 3 * u * t * t * c.y + t * t * t * d.y));
    }
}

// SVG arc (endpoint form) -> points, per the SVG implementation notes (F.6.5).
inline void Arc(std::vector<ImVec2>& o, ImVec2 p0, float rx, float ry, float phiDeg, bool large, bool sweep, ImVec2 p1) {
    if (rx == 0 || ry == 0 || (p0.x == p1.x && p0.y == p1.y)) { o.push_back(p1); return; }
    rx = fabsf(rx); ry = fabsf(ry);
    const float phi = phiDeg * 3.14159265f / 180.0f, cp = cosf(phi), sp = sinf(phi);
    const float dx = (p0.x - p1.x) * 0.5f, dy = (p0.y - p1.y) * 0.5f;
    const float x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
    const float lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lam > 1) { const float s = sqrtf(lam); rx *= s; ry *= s; }
    const float num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const float den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    float co = den == 0 ? 0 : sqrtf(fmaxf(0.0f, num / den));
    if (large == sweep) co = -co;
    const float cxp = co * (rx * y1 / ry), cyp = co * -(ry * x1 / rx);
    const float cx = cp * cxp - sp * cyp + (p0.x + p1.x) * 0.5f, cy = sp * cxp + cp * cyp + (p0.y + p1.y) * 0.5f;
    auto ang = [](float ux, float uy, float vx, float vy) {
        const float a = atan2f(ux * vy - uy * vx, ux * vx + uy * vy);
        return a;
    };
    const float th1 = ang(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry);
    float dth = ang((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
    if (!sweep && dth > 0) dth -= 6.28318531f;
    if (sweep && dth < 0) dth += 6.28318531f;
    const int n = (int)fmaxf(6.0f, ceilf(fabsf(dth) / 0.2f));
    for (int i = 1; i <= n; ++i) {
        const float t = th1 + dth * (float)i / n;
        const float ex = rx * cosf(t), ey = ry * sinf(t);
        o.push_back(ImVec2(cp * ex - sp * ey + cx, sp * ex + cp * ey + cy));
    }
}

} // namespace detail

// Parses SVG path data into flattened sub-paths (in the path's own units).
inline std::vector<SubPath> Parse(const char* d) {
    std::vector<SubPath> out;
    detail::Reader r{ d };
    ImVec2 cur(0, 0), start(0, 0), lastC(0, 0);
    char cmd = 0, prev = 0;
    auto beginSub = [&](ImVec2 p) { out.emplace_back(); out.back().pts.push_back(p); cur = start = p; };
    auto ensure = [&]() { if (out.empty() || out.back().closed) { SubPath s; s.pts.push_back(cur); if (!out.empty() && out.back().closed) start = cur; out.push_back(s); } };
    while (!r.AtEnd()) {
        char c;
        if (r.Command(c)) cmd = c; else if (cmd == 0) break;   // numbers without a command: stop
        else if (cmd == 'M') cmd = 'L'; else if (cmd == 'm') cmd = 'l';   // implicit lineto after moveto
        const bool rel = cmd >= 'a' && cmd <= 'z';
        const char up = (char)(rel ? cmd - 32 : cmd);
        float a[7];
        auto need = [&](int n) { for (int i = 0; i < n; ++i) if (!r.Number(a[i])) return false; return true; };
        if (up == 'Z') {
            if (!out.empty()) { out.back().closed = true; cur = start; }
            prev = 'Z';
            continue;
        }
        if (up == 'M') { if (!need(2)) break; ImVec2 p(a[0] + (rel ? cur.x : 0), a[1] + (rel ? cur.y : 0)); beginSub(p); }
        else if (up == 'L') { if (!need(2)) break; ensure(); ImVec2 p(a[0] + (rel ? cur.x : 0), a[1] + (rel ? cur.y : 0)); out.back().pts.push_back(p); cur = p; }
        else if (up == 'H') { if (!need(1)) break; ensure(); ImVec2 p(a[0] + (rel ? cur.x : 0), cur.y); out.back().pts.push_back(p); cur = p; }
        else if (up == 'V') { if (!need(1)) break; ensure(); ImVec2 p(cur.x, a[0] + (rel ? cur.y : 0)); out.back().pts.push_back(p); cur = p; }
        else if (up == 'C') {
            if (!need(6)) break; ensure();
            const float ox = rel ? cur.x : 0, oy = rel ? cur.y : 0;
            ImVec2 c1(a[0] + ox, a[1] + oy), c2(a[2] + ox, a[3] + oy), p(a[4] + ox, a[5] + oy);
            detail::Cubic(out.back().pts, cur, c1, c2, p); lastC = c2; cur = p;
        } else if (up == 'S') {
            if (!need(4)) break; ensure();
            const float ox = rel ? cur.x : 0, oy = rel ? cur.y : 0;
            ImVec2 c1 = (prev == 'C' || prev == 'S') ? ImVec2(2 * cur.x - lastC.x, 2 * cur.y - lastC.y) : cur;
            ImVec2 c2(a[0] + ox, a[1] + oy), p(a[2] + ox, a[3] + oy);
            detail::Cubic(out.back().pts, cur, c1, c2, p); lastC = c2; cur = p;
        } else if (up == 'Q' || up == 'T') {
            ensure();
            ImVec2 q, p;
            const float ox = rel ? cur.x : 0, oy = rel ? cur.y : 0;
            if (up == 'Q') { if (!need(4)) break; q = ImVec2(a[0] + ox, a[1] + oy); p = ImVec2(a[2] + ox, a[3] + oy); }
            else { if (!need(2)) break; q = (prev == 'Q' || prev == 'T') ? ImVec2(2 * cur.x - lastC.x, 2 * cur.y - lastC.y) : cur; p = ImVec2(a[0] + ox, a[1] + oy); }
            // quadratic -> cubic
            ImVec2 c1(cur.x + 2.0f / 3.0f * (q.x - cur.x), cur.y + 2.0f / 3.0f * (q.y - cur.y));
            ImVec2 c2(p.x + 2.0f / 3.0f * (q.x - p.x), p.y + 2.0f / 3.0f * (q.y - p.y));
            detail::Cubic(out.back().pts, cur, c1, c2, p); lastC = q; cur = p;
        } else if (up == 'A') {
            float rx = 0, ry = 0, rot = 0; bool large = false, sweep = false, ok;
            ok = r.Number(rx) && r.Number(ry) && r.Number(rot) && r.Flag(large) && r.Flag(sweep) && r.Number(a[0]) && r.Number(a[1]);
            if (!ok) break;
            ensure();
            ImVec2 p(a[0] + (rel ? cur.x : 0), a[1] + (rel ? cur.y : 0));
            detail::Arc(out.back().pts, cur, rx, ry, rot, large, sweep, p); cur = p;
        } else break;
        prev = up;
    }
    return out;
}

// Draws SVG path data with its 0..`view` square placed at `pos`, `size` pixels wide. `stroke` and `fill` are ImU32 colours (0 =
// none); `strokeWidth` is in path units (2 on a 24 grid is the usual icon weight).
inline void Draw(ImDrawList* dl, const char* d, ImVec2 pos, float size, ImU32 stroke, float strokeWidth = 2.0f, ImU32 fill = 0, float view = 24.0f) {
    static std::map<const char*, std::vector<SubPath>> cache;   // string literals are stable, so the pointer is a good key
    auto it = cache.find(d);
    if (it == cache.end()) it = cache.emplace(d, Parse(d)).first;
    const float s = size / view;
    std::vector<ImVec2> tmp;
    for (const SubPath& sp : it->second) {
        if (sp.pts.size() < 2) continue;
        tmp.resize(sp.pts.size());
        for (size_t i = 0; i < sp.pts.size(); ++i) tmp[i] = ImVec2(pos.x + sp.pts[i].x * s, pos.y + sp.pts[i].y * s);
        if (fill && sp.closed && tmp.size() >= 3) {
            // the concave (ear-clipping) fill needs a clean polygon: drop repeated points, including a last point equal to the first
            std::vector<ImVec2> poly; poly.reserve(tmp.size());
            for (const ImVec2& q : tmp) if (poly.empty() || fabsf(q.x - poly.back().x) + fabsf(q.y - poly.back().y) > 0.02f) poly.push_back(q);
            while (poly.size() > 1 && fabsf(poly.front().x - poly.back().x) + fabsf(poly.front().y - poly.back().y) <= 0.02f) poly.pop_back();
            // and a consistent winding (clockwise on screen)
            float area = 0; for (size_t i = 0; i < poly.size(); ++i) { const ImVec2 &a = poly[i], &b = poly[(i + 1) % poly.size()]; area += a.x * b.y - b.x * a.y; }
            if (area < 0) std::reverse(poly.begin(), poly.end());
            if (poly.size() >= 3) dl->AddConcavePolyFilled(poly.data(), (int)poly.size(), fill);
        }
        if (stroke && strokeWidth > 0) {
            dl->AddPolyline(tmp.data(), (int)tmp.size(), stroke, sp.closed ? ImDrawFlags_Closed : ImDrawFlags_None, strokeWidth * s);
            if (!sp.closed) {   // round caps
                const float r = strokeWidth * s * 0.5f;
                if (r >= 0.9f) { dl->AddCircleFilled(tmp.front(), r, stroke, 8); dl->AddCircleFilled(tmp.back(), r, stroke, 8); }
            }
        }
    }
}

} // namespace svg

// The bundled icons (24 x 24 grid).
namespace icons {
inline constexpr const char* kHeart      = "M19 14c1.49-1.46 3-3.21 3-5.5A5.5 5.5 0 0 0 16.5 3c-1.76 0-3 .5-4.5 2-1.5-1.5-2.74-2-4.5-2A5.5 5.5 0 0 0 2 8.5c0 2.3 1.5 4.05 3 5.5l7 7Z";
inline constexpr const char* kPlus       = "M5 12h14M12 5v14";
inline constexpr const char* kDownload   = "M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M7 10l5 5 5-5M12 15V3";
inline constexpr const char* kFolder     = "M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z";
inline constexpr const char* kReset      = "M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8M3 3v5h5";
inline constexpr const char* kChevronDown  = "m6 9 6 6 6-6";
inline constexpr const char* kChevronRight = "m9 18 6-6-6-6";
inline constexpr const char* kCheck      = "M20 6 9 17l-5-5";
inline constexpr const char* kClose      = "M18 6 6 18M6 6l12 12";
inline constexpr const char* kExternal   = "M15 3h6v6M10 14 21 3M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6";
inline constexpr const char* kInfo       = "M12 22a10 10 0 1 0 0-20 10 10 0 0 0 0 20ZM12 16v-4M12 8h.01";
inline constexpr const char* kCoffee     = "M10 2v2M14 2v2M16 8a1 1 0 0 1 1 1v8a4 4 0 0 1-4 4H7a4 4 0 0 1-4-4V9a1 1 0 0 1 1-1h14a4 4 0 1 1 0 8h-1M6 2v2";
inline constexpr const char* kPower      = "M12 2v10M18.4 6.6a9 9 0 1 1-12.77.04";
inline constexpr const char* kPackage    = "m7.5 4.27 9 5.15M21 8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16ZM3.3 7 12 12l8.7-5M12 22V12";
inline constexpr const char* kSliders    = "M21 4h-7M10 4H3M21 12h-9M8 12H3M21 20h-5M12 20H3M14 2v4M8 10v4M16 18v4";
// The Echo mark: a frame with two fainter copies stepping up and to the right behind it (also the app icon).
inline constexpr const char* kEchoBack   = "M13.7 1.5h6.6a2.2 2.2 0 0 1 2.2 2.2v6.6a2.2 2.2 0 0 1-2.2 2.2h-6.6a2.2 2.2 0 0 1-2.2-2.2v-6.6a2.2 2.2 0 0 1 2.2-2.2Z";
inline constexpr const char* kEchoMid    = "M9.2 6h6.6a2.2 2.2 0 0 1 2.2 2.2v6.6a2.2 2.2 0 0 1-2.2 2.2h-6.6a2.2 2.2 0 0 1-2.2-2.2v-6.6a2.2 2.2 0 0 1 2.2-2.2Z";
inline constexpr const char* kEchoFront  = "M4.7 10.5h6.6a2.2 2.2 0 0 1 2.2 2.2v6.6a2.2 2.2 0 0 1-2.2 2.2h-6.6a2.2 2.2 0 0 1-2.2-2.2v-6.6a2.2 2.2 0 0 1 2.2-2.2Z";
inline constexpr const char* kTrash      = "M3 6h18M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M10 11v6M14 11v6";
inline constexpr const char* kFile       = "M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7ZM14 2v4a2 2 0 0 0 2 2h4M10 9H8M16 13H8M16 17H8";
inline constexpr const char* kSave       = "M15.2 3a2 2 0 0 1 1.4.6l3.8 3.8a2 2 0 0 1 .6 1.4V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2ZM17 21v-7a1 1 0 0 0-1-1H8a1 1 0 0 0-1 1v7M7 3v4a1 1 0 0 0 1 1h7";
inline constexpr const char* kPlay       = "M6 3 20 12 6 21Z";
// The three plugins' own shapes (the same as their icon.svg files): a sparkle (Neural Rendering), a small frame grown into a large one (DLSS 4
// Upscaler), and the same thrown with motion lines (FSR Upscaler).
inline constexpr const char* kSparkles   = "M10 3.5 11.9 8.6 17 10.5 11.9 12.4 10 17.5 8.1 12.4 3 10.5 8.1 8.6ZM18.5 15 19.3 17.2 21.5 18 19.3 18.8 18.5 21 17.7 18.8 15.5 18 17.7 17.2ZM19 3v4M17 5h4";
inline constexpr const char* kUpscale    = "M4 13.5h6.5V20H4ZM13.5 4H20v6.5M20 4 13 11M4 9.5V5a1 1 0 0 1 1-1h4.5M20 14.5V19a1 1 0 0 1-1 1h-4.5";
inline constexpr const char* kUpscaleFast = "M9.5 9.5H20V20H9.5ZM3 13h3.5M3 16.5h3.5M5 9.5h1.5M4 4h5v5H4ZM9 9l4.5 4.5M13.5 10.5v3h-3";
inline constexpr const char* kKeyboard   = "M4 6h16a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2ZM6 10h.01M10 10h.01M14 10h.01M18 10h.01M8 14h8";
inline constexpr const char* kMonitor    = "M4 3h16a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2ZM8 21h8M12 17v4";
inline constexpr const char* kEye        = "M2 12s3-7 10-7 10 7 10 7-3 7-10 7-10-7-10-7ZM12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6Z";
inline constexpr const char* kCircle     = "M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18Z";
inline constexpr const char* kVideo      = "M3 6h11a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2ZM16 10.5l6-3.5v10l-6-3.5";
inline constexpr const char* kGauge      = "M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18ZM12 12l4-4M7 12h.01M12 7h.01M17 12h.01";
inline constexpr const char* kAlert      = "M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0ZM12 9v4M12 17h.01";
} // namespace icons

} // namespace eam::ui
