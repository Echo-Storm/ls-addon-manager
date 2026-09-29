// An "oracle" motion field for offline experiments: dense motion between two FULL-SIZE frames (the recording's own, which the upscaler under test never
// sees), found by hierarchical block matching (8x8 blocks on luma pyramids, coarse to fine, a sub-pixel step at the end). What the upscalers are given in
// place of our real-time estimate, to answer: is it the accuracy of the motion that stops a temporal upscaler from adding detail, or is there nothing to
// gain without the game's camera jitter? (nr_sreval oracle=1.) Slow (CPU, a second or so a 4K frame); not for the addon.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace nr::oracle {

struct Luma { uint32_t w = 0, h = 0; std::vector<float> v; float At(int x, int y) const { x = std::clamp(x, 0, int(w) - 1); y = std::clamp(y, 0, int(h) - 1); return v[static_cast<size_t>(y) * w + x]; } };

inline Luma FromRgba8(const std::vector<uint8_t>& p, uint32_t W, uint32_t H) {
    Luma l; l.w = W; l.h = H; l.v.resize(static_cast<size_t>(W) * H);
    for (size_t i = 0; i < l.v.size(); ++i) l.v[i] = 0.2126f * p[i * 4] + 0.7152f * p[i * 4 + 1] + 0.0722f * p[i * 4 + 2];
    return l;
}
inline Luma Half(const Luma& a) {
    Luma l; l.w = std::max(1u, a.w / 2); l.h = std::max(1u, a.h / 2); l.v.resize(static_cast<size_t>(l.w) * l.h);
    for (uint32_t y = 0; y < l.h; ++y) for (uint32_t x = 0; x < l.w; ++x)
        l.v[static_cast<size_t>(y) * l.w + x] = 0.25f * (a.At(2 * x, 2 * y) + a.At(2 * x + 1, 2 * y) + a.At(2 * x, 2 * y + 1) + a.At(2 * x + 1, 2 * y + 1));
    return l;
}

// One vector (dx, dy) per 8x8 block of the finest level: where the block of `cur` is in `prev` (prev(x + dx, y + dy) matches cur(x, y)), in pixels of `cur`.
struct Field { uint32_t gx = 0, gy = 0; std::vector<float> dx, dy; };

inline Field Estimate(const Luma& cur0, const Luma& prev0, int levels = 4) {
    std::vector<Luma> cur{ cur0 }, prev{ prev0 };
    for (int i = 1; i < levels; ++i) { cur.push_back(Half(cur.back())); prev.push_back(Half(prev.back())); }
    Field field;
    for (int lv = levels - 1; lv >= 0; --lv) {
        const Luma& c = cur[lv]; const Luma& p = prev[lv];
        Field f; f.gx = (c.w + 7) / 8; f.gy = (c.h + 7) / 8; f.dx.assign(static_cast<size_t>(f.gx) * f.gy, 0.0f); f.dy = f.dx;
        const bool coarsest = lv == levels - 1, finest = lv == 0;
        const int R = coarsest ? 8 : 2;   // the search around the parent's vector (doubled)
        for (uint32_t by = 0; by < f.gy; ++by) for (uint32_t bx = 0; bx < f.gx; ++bx) {
            float gx0 = 0, gy0 = 0;
            if (!coarsest) {   // the parent's vector for this block, doubled (the block above-left in the coarser grid)
                const uint32_t px = std::min(field.gx - 1, bx / 2), py = std::min(field.gy - 1, by / 2);
                gx0 = 2.0f * field.dx[static_cast<size_t>(py) * field.gx + px]; gy0 = 2.0f * field.dy[static_cast<size_t>(py) * field.gx + px];
            }
            const int ox = static_cast<int>(std::lround(gx0)), oy = static_cast<int>(std::lround(gy0));
            const int x0 = static_cast<int>(bx) * 8, y0 = static_cast<int>(by) * 8;
            auto cost = [&](int sx, int sy) { float s = 0; for (int j = 0; j < 8; ++j) for (int i = 0; i < 8; ++i) s += std::fabs(c.At(x0 + i, y0 + j) - p.At(x0 + i + sx, y0 + j + sy)); return s; };
            float best = 1e30f; int bsx = ox, bsy = oy;
            for (int sy = -R; sy <= R; ++sy) for (int sx = -R; sx <= R; ++sx) {
                const float k = cost(ox + sx, oy + sy) + 0.02f * float(std::abs(sx) + std::abs(sy));   // (a hair of preference for the parent's vector)
                if (k < best) { best = k; bsx = ox + sx; bsy = oy + sy; }
            }
            float fx = float(bsx), fy = float(bsy);
            if (finest) {   // a fraction of a pixel: a parabola through the costs either side, per axis (mean squared difference is smooth around the match)
                auto sq = [&](int sx, int sy) { float s = 0; for (int j = 0; j < 8; ++j) for (int i = 0; i < 8; ++i) { const float d = c.At(x0 + i, y0 + j) - p.At(x0 + i + sx, y0 + j + sy); s += d * d; } return s; };
                const float c0 = sq(bsx, bsy), xm = sq(bsx - 1, bsy), xp = sq(bsx + 1, bsy), ym = sq(bsx, bsy - 1), yp = sq(bsx, bsy + 1);
                const float ax = xm + xp - 2 * c0, ay = ym + yp - 2 * c0;
                if (ax > 1e-3f) fx += std::clamp(0.5f * (xm - xp) / ax, -0.5f, 0.5f);
                if (ay > 1e-3f) fy += std::clamp(0.5f * (ym - yp) / ay, -0.5f, 0.5f);
            }
            f.dx[static_cast<size_t>(by) * f.gx + bx] = fx; f.dy[static_cast<size_t>(by) * f.gx + bx] = fy;
        }
        field = std::move(f);
    }
    return field;
}

// The field as motion vectors for an upscaler's input of iw x ih pixels (the frames were shrunk by `scale`): per input pixel, bilinear between the blocks'
// vectors, in input pixels (the frame's size divided by the scale), as half floats (x, y, 0, 0).
inline void ToInputVectors(const Field& f, uint32_t W, uint32_t H, uint32_t iw, uint32_t ih, std::vector<float>& out) {
    out.assign(static_cast<size_t>(iw) * ih * 2, 0.0f);
    const float sx = float(W) / float(iw), sy = float(H) / float(ih);   // frame pixels per input pixel
    for (uint32_t y = 0; y < ih; ++y) for (uint32_t x = 0; x < iw; ++x) {
        const float fx = std::clamp(((x + 0.5f) * sx) / 8.0f - 0.5f, 0.0f, float(f.gx - 1)), fy = std::clamp(((y + 0.5f) * sy) / 8.0f - 0.5f, 0.0f, float(f.gy - 1));
        const uint32_t x0 = static_cast<uint32_t>(fx), y0 = static_cast<uint32_t>(fy), x1 = std::min(x0 + 1, f.gx - 1), y1 = std::min(y0 + 1, f.gy - 1);
        const float ax = fx - x0, ay = fy - y0;
        auto at = [&](const std::vector<float>& v, uint32_t xx, uint32_t yy) { return v[static_cast<size_t>(yy) * f.gx + xx]; };
        const float dx = (at(f.dx, x0, y0) * (1 - ax) + at(f.dx, x1, y0) * ax) * (1 - ay) + (at(f.dx, x0, y1) * (1 - ax) + at(f.dx, x1, y1) * ax) * ay;
        const float dy = (at(f.dy, x0, y0) * (1 - ax) + at(f.dy, x1, y0) * ax) * (1 - ay) + (at(f.dy, x0, y1) * (1 - ax) + at(f.dy, x1, y1) * ax) * ay;
        out[(static_cast<size_t>(y) * iw + x) * 2] = dx / sx; out[(static_cast<size_t>(y) * iw + x) * 2 + 1] = dy / sy;
    }
}

} // namespace nr::oracle
