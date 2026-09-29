// The offline evaluation tools' shared pieces (nr_fgeval, nr_sreval): arguments, a recording's frames as 8-bit RGBA in their SDR view,
// scores and pictures.
#pragma once
#include <windows.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "addon/lsrec.h"

namespace nr::eval {

inline std::wstring Wide(const char* s) { const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0); std::wstring w(n > 0 ? n - 1 : 0, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n); return w; }
inline int Arg(int argc, char** argv, const char* key, int fallback) {
    const size_t k = strlen(key);
    for (int i = 3; i < argc; ++i) if (!strncmp(argv[i], key, k) && argv[i][k] == '=') return atoi(argv[i] + k + 1);
    return fallback;
}
inline std::string ArgText(int argc, char** argv, const char* key) {
    const size_t k = strlen(key);
    for (int i = 3; i < argc; ++i) if (!strncmp(argv[i], key, k) && argv[i][k] == '=') return argv[i] + k + 1;
    return {};
}

// A frame of the recording as 8-bit RGBA in its SDR view. Light (the upscalers' HDR frames, 1 = the SDR white) and scRGB are rolled off
// above 0.75 as the screenshots do, then sRGB-encoded; an SDR view is taken as it is; 8-bit frames as they are (BGRA swapped to RGBA).
inline bool ToRgba8(const nr::lsrec::FileHeader& h, const std::vector<uint8_t>& px, std::vector<uint8_t>& out) {
    const size_t n = static_cast<size_t>(h.width) * h.height;
    out.resize(n * 4);
    if (h.bytesPerPixel == 4) {
        const bool bgra = h.format == DXGI_FORMAT_B8G8R8A8_UNORM || h.format == DXGI_FORMAT_B8G8R8X8_UNORM;
        for (size_t i = 0; i < n; ++i) {
            out[i * 4 + 0] = px[i * 4 + (bgra ? 2 : 0)]; out[i * 4 + 1] = px[i * 4 + 1]; out[i * 4 + 2] = px[i * 4 + (bgra ? 0 : 2)]; out[i * 4 + 3] = 255;
        }
        return true;
    }
    if (h.bytesPerPixel != 8) return false;
    const uint16_t* in = reinterpret_cast<const uint16_t*>(px.data());
    const float scale = h.content == nr::lsrec::kOwnEncoding ? 80.0f / 200.0f : 1.0f;   // scRGB of its own: 1.0 = 80 nits, SDR white taken as 200
    for (size_t i = 0; i < n; ++i) for (int c = 0; c < 3; ++c) {
        float v = DirectX::PackedVector::XMConvertHalfToFloat(in[i * 4 + c]);
        if (h.content != nr::lsrec::kSdrView) {
            v = std::max(v * scale, 0.0f);
            if (v > 0.75f) v = 0.75f + 0.25f * (1.0f - std::exp(-(v - 0.75f) / 0.25f));
            v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        }
        out[i * 4 + c] = static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        if (c == 2) out[i * 4 + 3] = 255;
    }
    return true;
}

// Peak signal-to-noise ratio of two RGBA8 pictures over R, G and B (dB; higher is closer), and the mean absolute difference (levels).
inline double Psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, double* meanAbs) {
    double se = 0, ae = 0; size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4) for (int c = 0; c < 3; ++c) { const double d = double(a[i + c]) - double(b[i + c]); se += d * d; ae += std::abs(d); ++n; }
    if (meanAbs) *meanAbs = n ? ae / n : 0;
    const double mse = n ? se / n : 0;
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// The same, at a quarter of the size (4x4 averages): what is left is where things are, not their fine detail, so a double image (ghosting)
// still costs and a leaf a pixel off does not. W: the pictures' width.
inline double PsnrCoarse(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t W) {
    const uint32_t H = static_cast<uint32_t>(a.size() / 4 / W), w = W / 4, h = H / 4;
    double se = 0; size_t n = 0;
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) for (int c = 0; c < 3; ++c) {
        double sa = 0, sb = 0;
        for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) { const size_t p = ((static_cast<size_t>(y) * 4 + j) * W + x * 4 + i) * 4 + c; sa += a[p]; sb += b[p]; }
        const double d = (sa - sb) / 16.0; se += d * d; ++n;
    }
    const double mse = n ? se / n : 0;
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// Steadiness (shimmer): how far the picture's change since the frame before strays from the true change (dB; higher is steadier).
// A picture that flickers where the truth holds still, or crawls along edges, costs; a picture that changes as the truth does costs
// nothing even if it is soft. a, aPrev: this and the frame before's picture; b, bPrev: the same frames' truth. Luma (Rec. 709) only.
inline double PsnrTemporal(const std::vector<uint8_t>& a, const std::vector<uint8_t>& aPrev, const std::vector<uint8_t>& b, const std::vector<uint8_t>& bPrev) {
    auto luma = [](const std::vector<uint8_t>& p, size_t i) { return 0.2126 * p[i] + 0.7152 * p[i + 1] + 0.0722 * p[i + 2]; };
    double se = 0; size_t n = 0;
    for (size_t i = 0; i < a.size(); i += 4) { const double d = (luma(a, i) - luma(aPrev, i)) - (luma(b, i) - luma(bPrev, i)); se += d * d; ++n; }
    const double mse = n ? se / n : 0;
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// No-reference scores, for judging a picture with no true one to compare with (1:1, where the game's own frame is the aliased input):
//   Flicker: over three frames in a row (the one before, this one, the one after), how far the middle one is from the average of its neighbours,
//            in levels of 255 on luma; a steady picture, or one that changes evenly, scores 0, shimmer scores what it shimmers;
//   Detail:  the mean step between neighbouring pixels (right and down), on luma: what fine detail and sharpness are left (a blur lowers it).
// Both on every second pixel each way (quick). Luma is Rec. 709 of the RGBA8.
inline double Flicker(const std::vector<uint8_t>& before, const std::vector<uint8_t>& now, const std::vector<uint8_t>& after, uint32_t W) {
    const uint32_t H = static_cast<uint32_t>(now.size() / 4 / W);
    auto luma = [&](const std::vector<uint8_t>& p, size_t i) { return 0.2126 * p[i] + 0.7152 * p[i + 1] + 0.0722 * p[i + 2]; };
    double sum = 0; size_t n = 0;
    for (uint32_t y = 0; y < H; y += 2) for (uint32_t x = 0; x < W; x += 2) {
        const size_t i = (static_cast<size_t>(y) * W + x) * 4;
        sum += std::abs(luma(now, i) - 0.5 * (luma(before, i) + luma(after, i))); ++n;
    }
    return n ? sum / n : 0.0;
}
inline double Detail(const std::vector<uint8_t>& p, uint32_t W) {
    const uint32_t H = static_cast<uint32_t>(p.size() / 4 / W);
    auto luma = [&](size_t i) { return 0.2126 * p[i] + 0.7152 * p[i + 1] + 0.0722 * p[i + 2]; };
    double sum = 0; size_t n = 0;
    for (uint32_t y = 0; y + 1 < H; y += 2) for (uint32_t x = 0; x + 1 < W; x += 2) {
        const size_t i = (static_cast<size_t>(y) * W + x) * 4;
        sum += std::abs(luma(i) - luma(i + 4)) + std::abs(luma(i) - luma(i + static_cast<size_t>(W) * 4)); ++n;
    }
    return n ? sum / n : 0.0;
}

inline bool WriteBmp(const std::wstring& path, const std::vector<const std::vector<uint8_t>*>& tiles, uint32_t w, uint32_t h) {
    const uint32_t W = w * static_cast<uint32_t>(tiles.size()), rowBytes = W * 3, pad = (4 - rowBytes % 4) % 4;
    FILE* f = _wfopen(path.c_str(), L"wb"); if (!f) return false;
    const uint32_t imageSize = (rowBytes + pad) * h, fileSize = 54 + imageSize;
    uint8_t hdr[54] = { 'B', 'M' };
    auto put32 = [&](int at, uint32_t v) { memcpy(hdr + at, &v, 4); };
    put32(2, fileSize); put32(10, 54); put32(14, 40); put32(18, W); put32(22, h); hdr[26] = 1; hdr[28] = 24; put32(34, imageSize);
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> row(rowBytes + pad, 0);
    for (int y = static_cast<int>(h) - 1; y >= 0; --y) {
        for (size_t t = 0; t < tiles.size(); ++t) for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = tiles[t]->data() + (static_cast<size_t>(y) * w + x) * 4;
            uint8_t* o = row.data() + (t * w + x) * 3; o[0] = p[2]; o[1] = p[1]; o[2] = p[0];
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    return true;
}

} // namespace nr::eval
