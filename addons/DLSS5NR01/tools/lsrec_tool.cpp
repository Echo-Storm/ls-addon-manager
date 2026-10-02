// nr_lsrec: looks into recordings (.lsrec, addon/lsrec.h).
//   nr_lsrec info <file>                                   what is in it: size, format, frames, time, frame rate, frames left out
//   nr_lsrec export <file> <folder> [every=N] [first=N] [count=N]   frames as BMP pictures (HDR ones tone-mapped as the screenshots are)
//   nr_lsrec flicker <file> [first=N] [count=N]            how much the picture changes from frame to frame (levels of 255), by the kind of pair, the biggest steps, and whether the change pulses at a period
//   nr_lsrec make <file> <width> <height> <frames> [fps=N] [hdr=1]  a made-up recording of a moving picture (for the tests, no game needed); hdr=1: half-float light
//                                                     (1 = the SDR white) with small bright glints (1.2 to 1.9), the kind of highlight a sharpening pass must not turn into a speck
#include "addon/lsrec.h"
#include "addon/screenshot.h"
#include <windows.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace nr::lsrec;

static std::wstring Wide(const char* s) {
    std::wstring w(strlen(s), L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), static_cast<int>(w.size()) + 1);
    w.resize(n > 0 ? n - 1 : 0);
    return w;
}
static int Arg(int argc, char** argv, int from, const char* key, int fallback) {
    const size_t n = strlen(key);
    for (int i = from; i < argc; ++i) if (!strncmp(argv[i], key, n) && argv[i][n] == '=') return atoi(argv[i] + n + 1);
    return fallback;
}
static const char* SourceName(uint32_t s) { return s == kCaptured ? "captured frames (frame generation on)" : s == kPresented ? "presented frames (frame generation off)" : s == kNisInput ? "NIS's input (an upscaler)" : "?"; }

static bool WriteBmp(const std::wstring& path, const uint8_t* bgra, uint32_t w, uint32_t h) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
    const uint32_t rowBytes = w * 4, imageBytes = rowBytes * h, fileBytes = 54 + imageBytes, offset = 54, infoBytes = 40;
    uint8_t fh[14] = { 'B', 'M' }; memcpy(fh + 2, &fileBytes, 4); memcpy(fh + 10, &offset, 4);
    uint8_t ih[40] = {}; memcpy(ih, &infoBytes, 4); const int32_t wv = static_cast<int32_t>(w), hv = -static_cast<int32_t>(h); memcpy(ih + 4, &wv, 4); memcpy(ih + 8, &hv, 4);
    const uint16_t planes = 1, bpp = 32; memcpy(ih + 12, &planes, 2); memcpy(ih + 14, &bpp, 2); memcpy(ih + 20, &imageBytes, 4);
    const bool ok = fwrite(fh, 14, 1, f) == 1 && fwrite(ih, 40, 1, f) == 1 && fwrite(bgra, imageBytes, 1, f) == 1;
    fclose(f);
    return ok;
}

static int Info(const char* file) {
    Reader r; std::string error;
    if (!r.Open(Wide(file), &error)) { printf("%s: %s\n", file, error.c_str()); return 2; }
    const FileHeader& h = r.Header();
    uint64_t compressed = 0, missed = 0;
    for (size_t i = 0; i < r.Count(); ++i) { compressed += r.FrameInfo(i).bytes; if (i) missed += r.FrameInfo(i).index - r.FrameInfo(i - 1).index - 1; }
    const double seconds = r.Count() > 1 && h.qpcFrequency ? static_cast<double>(r.FrameInfo(r.Count() - 1).qpc - r.FrameInfo(0).qpc) / h.qpcFrequency : 0.0;
    const double raw = static_cast<double>(h.width) * h.height * h.bytesPerPixel * r.Count();
    printf("%s\n  %ux%u, DXGI format %u (%u bytes a pixel), %s\n  game: %s\n  %zu frames over %.2f s (%.1f frames a second), %llu left out while recording\n"
           "  %.1f MB compressed, %.0f%% of the %.1f MB raw\n", file, h.width, h.height, h.format, h.bytesPerPixel, SourceName(h.source), h.game[0] ? h.game : "(not known)",
           r.Count(), seconds, seconds > 0 ? (r.Count() - 1) / seconds : 0.0, (unsigned long long)missed, compressed / 1048576.0, raw ? 100.0 * compressed / raw : 0.0, raw / 1048576.0);
    if (r.Count() != h.frameCount) printf("  the file is cut off: %zu of %u frames are there\n", r.Count(), h.frameCount);
    return 0;
}

static int Export(int argc, char** argv) {
    Reader r; std::string error;
    if (!r.Open(Wide(argv[2]), &error)) { printf("%s: %s\n", argv[2], error.c_str()); return 2; }
    const std::wstring folder = Wide(argv[3]);
    CreateDirectoryW(folder.c_str(), nullptr);
    const int every = std::max(1, Arg(argc, argv, 4, "every", 1)), first = std::max(0, Arg(argc, argv, 4, "first", 0));
    const int count = Arg(argc, argv, 4, "count", 1 << 30);
    const FileHeader& h = r.Header();
    std::vector<uint8_t> px, bgra(static_cast<size_t>(h.width) * h.height * 4);
    int written = 0;
    for (size_t i = first; i < r.Count() && written < count; i += every) {
        if (!r.Read(i, px)) { printf("frame %zu could not be read\n", i); return 3; }
        for (uint32_t y = 0; y < h.height; ++y)
            if ((h.content == kSdrView || h.content == kLight) && h.bytesPerPixel == 8) {   // half floats: the SDR view as it is; light rolled off as screenshots show HDR
                const uint16_t* in = reinterpret_cast<const uint16_t*>(px.data() + static_cast<size_t>(y) * h.width * 8);
                uint8_t* out = bgra.data() + static_cast<size_t>(y) * h.width * 4;
                for (uint32_t x = 0; x < h.width; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        float v = DirectX::PackedVector::XMConvertHalfToFloat(in[x * 4 + c]);
                        if (h.content == kLight) {   // light (1 = the SDR white): the screenshots' roll-off above 0.75, then sRGB
                            v = std::max(v, 0.0f);
                            if (v > 0.75f) v = 0.75f + 0.25f * (1.0f - std::exp(-(v - 0.75f) / 0.25f));
                            v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
                        }
                        out[x * 4 + (2 - c)] = static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);   // RGBA -> BGRA
                    }
                    out[x * 4 + 3] = 255;
                }
            } else if (!nr::screenshot::ToBgra8(static_cast<DXGI_FORMAT>(h.format), px.data() + static_cast<size_t>(y) * h.width * h.bytesPerPixel, h.width, bgra.data() + static_cast<size_t>(y) * h.width * 4)) {
                printf("format %u cannot be converted to a picture\n", h.format); return 3;
            }
        // (the presented frames with frame generation of our own say which they are: a frame made between, or a real one)
        const uint32_t tag = r.FrameInfo(i).tag;
        wchar_t name[48]; swprintf(name, 48, L"\\frame_%05zu%ls.bmp", i, tag == nr::lsrec::kMadeBetween ? L"_made" : tag == nr::lsrec::kReal ? L"_real" : L"");
        if (!WriteBmp(folder + name, bgra.data(), h.width, h.height)) { printf("could not write into %s\n", argv[3]); return 3; }
        ++written;
    }
    printf("%d frames written to %s\n", written, argv[3]);
    return 0;
}

// The host test's moving picture: blocks of hashed colours sliding right and down, as a camera pans.
static uint32_t Hash(uint32_t x, uint32_t y) { uint32_t h = x * 374761393u + y * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16); }
static int Make(int argc, char** argv) {
    const uint32_t w = static_cast<uint32_t>(atoi(argv[3])), h = static_cast<uint32_t>(atoi(argv[4])), n = static_cast<uint32_t>(atoi(argv[5]));
    const int fps = std::max(1, Arg(argc, argv, 6, "fps", 60));
    if (w < 64 || h < 64 || !n) { printf("width and height of at least 64, and some frames\n"); return 1; }
    const bool hdr = Arg(argc, argv, 6, "hdr", 0) != 0;
    std::vector<Frame> frames(n);
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * (hdr ? 8 : 4));
    for (uint32_t t = 0; t < n; ++t) {
        if (hdr) {
            uint16_t* out = reinterpret_cast<uint16_t*>(px.data());
            for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
                const double u = x + 0.5 - t * 3.3, v = y + 0.5 - t * 1.4;
                const int64_t bx = static_cast<int64_t>(std::floor(u / 12 + 100000)), by = static_cast<int64_t>(std::floor(v / 12 + 100000));
                const uint32_t c = Hash(static_cast<uint32_t>(bx), static_cast<uint32_t>(by));
                float rgb[3] = { 0.04f + 0.6f * ((c & 0xFF) / 255.0f), 0.04f + 0.6f * (((c >> 8) & 0xFF) / 255.0f), 0.04f + 0.6f * (((c >> 16) & 0xFF) / 255.0f) };
                // a glint: a 3x3 patch at the middle of some of the blocks, 1.2 to 1.9 times the SDR white
                const double fx = u / 12 - std::floor(u / 12), fy = v / 12 - std::floor(v / 12);
                if (Hash(static_cast<uint32_t>(bx) + 7u, static_cast<uint32_t>(by) + 3u) % 5u == 0 && fx > 0.375 && fx < 0.625 && fy > 0.375 && fy < 0.625) {
                    const float g = 1.2f + 0.1f * static_cast<float>(c % 8u); rgb[0] = rgb[1] = rgb[2] = g;
                }
                uint16_t* p = out + (static_cast<size_t>(y) * w + x) * 4;
                for (int k = 0; k < 3; ++k) p[k] = DirectX::PackedVector::XMConvertFloatToHalf(rgb[k]);
                p[3] = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
            }
            frames[t].header.index = t + 1; frames[t].header.qpc = static_cast<int64_t>(t) * 10000000 / fps; frames[t].header.rawBytes = w * h * 8;
            Compress(px.data(), w * 2, h, w * 8, frames[t].data);
            continue;
        }
        for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
            const double u = x + 0.5 - t * 3.3, v = y + 0.5 - t * 1.4;
            const uint32_t c = Hash(static_cast<uint32_t>(static_cast<int64_t>(std::floor(u / 12 + 100000))), static_cast<uint32_t>(static_cast<int64_t>(std::floor(v / 12 + 100000))));
            uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>(40 + (c & 0x9F)); p[1] = static_cast<uint8_t>(40 + ((c >> 8) & 0x9F)); p[2] = static_cast<uint8_t>(40 + ((c >> 16) & 0x9F)); p[3] = 255;
        }
        frames[t].header.index = t + 1; frames[t].header.qpc = static_cast<int64_t>(t) * 10000000 / fps; frames[t].header.rawBytes = w * h * 4;
        Compress(px.data(), w, h, w * 4, frames[t].data);
    }
    FileHeader header; header.width = w; header.height = h; header.format = hdr ? 10 : 87; header.bytesPerPixel = hdr ? 8 : 4; header.source = kPresented; header.qpcFrequency = 10000000;
    if (hdr) header.content = kLight;
    snprintf(header.game, sizeof header.game, "nr_lsrec make");
    std::vector<const Frame*> list; for (const Frame& f : frames) list.push_back(&f);
    std::string error;
    if (!Write(Wide(argv[2]), header, list, &error)) { printf("%s: %s\n", argv[2], error.c_str()); return 3; }
    printf("%u frames of %ux%u at %d frames a second written to %s\n", n, w, h, fps, argv[2]);
    return 0;
}


// ---- flicker: how much the picture changes from one frame to the next, for a recording of what is shown (or of anything): the mean change of luma (levels of 255, on every 4th pixel each way), split by
// the tag of the pair (a made frame after a real one and the other way, when the recording says), its spread, the biggest steps, and whether the change repeats at some period (a pulse every Nth frame)
static bool LumaOf(const FileHeader& h, const std::vector<uint8_t>& px, std::vector<float>& luma) {
    const uint32_t sw = (h.width + 3) / 4, sh = (h.height + 3) / 4;
    luma.assign(static_cast<size_t>(sw) * sh, 0.0f);
    const bool half = (h.content == kSdrView || h.content == kLight) && h.bytesPerPixel == 8;
    std::vector<uint8_t> row(static_cast<size_t>(h.width) * 4);
    for (uint32_t y = 0; y < h.height; y += 4) {
        const uint8_t* src = px.data() + static_cast<size_t>(y) * h.width * h.bytesPerPixel;
        if (!half && !nr::screenshot::ToBgra8(static_cast<DXGI_FORMAT>(h.format), src, h.width, row.data())) return false;
        for (uint32_t x = 0; x < h.width; x += 4) {
            float c[3];
            if (half) {
                const uint16_t* in = reinterpret_cast<const uint16_t*>(src) + static_cast<size_t>(x) * 4;
                for (int k = 0; k < 3; ++k) {
                    float v = DirectX::PackedVector::XMConvertHalfToFloat(in[k]);
                    if (h.content == kLight) { v = std::max(v, 0.0f); if (v > 0.75f) v = 0.75f + 0.25f * (1.0f - std::exp(-(v - 0.75f) / 0.25f)); v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; }
                    c[k] = std::clamp(v, 0.0f, 1.0f) * 255.0f;
                }
            } else { c[0] = row[x * 4 + 2]; c[1] = row[x * 4 + 1]; c[2] = row[x * 4]; }
            luma[static_cast<size_t>(y / 4) * sw + x / 4] = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
        }
    }
    return true;
}

static int Flicker(int argc, char** argv) {
    Reader r; std::string error;
    if (!r.Open(Wide(argv[2]), &error)) { printf("%s: %s\n", argv[2], error.c_str()); return 2; }
    const FileHeader& h = r.Header();
    const size_t first = static_cast<size_t>(std::max(0, Arg(argc, argv, 3, "first", 0)));
    const size_t count = static_cast<size_t>(std::max(2, Arg(argc, argv, 3, "count", 1 << 30)));
    std::vector<uint8_t> px; std::vector<float> prev, cur;
    std::vector<double> steps; std::vector<uint32_t> tags; std::vector<double> dts;
    uint32_t prevTag = 0; int64_t prevQpc = 0;
    for (size_t i = first; i < r.Count() && i < first + count; ++i) {
        if (!r.Read(i, px) || !LumaOf(h, px, cur)) { printf("frame %zu could not be read or converted\n", i); return 3; }
        const uint32_t tag = r.FrameInfo(i).tag; const int64_t qpc = r.FrameInfo(i).qpc;
        if (!prev.empty()) {
            double sum = 0; for (size_t k = 0; k < cur.size(); ++k) sum += std::fabs(cur[k] - prev[k]);
            steps.push_back(sum / cur.size());
            tags.push_back(prevTag * 4 + tag);
            dts.push_back(h.qpcFrequency ? (qpc - prevQpc) * 1000.0 / h.qpcFrequency : 0.0);
        }
        prev.swap(cur); prevTag = tag; prevQpc = qpc;
    }
    if (steps.size() < 3) { printf("too few frames to say anything\n"); return 0; }
    auto stats = [&](const std::vector<double>& v, const char* name, const char* unit = "levels of 255") {
        if (v.empty()) return;
        std::vector<double> s = v; std::sort(s.begin(), s.end());
        double m = 0; for (double x : v) m += x; m /= v.size();
        printf("  %-22s n %4zu  mean %.3f  p50 %.3f  p95 %.3f  max %.3f %s\n", name, v.size(), m, s[s.size() / 2], s[static_cast<size_t>(0.95 * (s.size() - 1))], s.back(), unit);
    };
    printf("%s: %zu frame steps (%ux%u, every 4th pixel each way)\n", argv[2], steps.size(), h.width, h.height);
    stats(steps, "all steps");
    std::vector<double> realToMade, madeToReal, same;
    for (size_t i = 0; i < steps.size(); ++i) {
        const uint32_t a = tags[i] / 4, b = tags[i] % 4;
        if (a == nr::lsrec::kReal && b == nr::lsrec::kMadeBetween) realToMade.push_back(steps[i]);
        else if (a == nr::lsrec::kMadeBetween && b == nr::lsrec::kReal) madeToReal.push_back(steps[i]);
        else same.push_back(steps[i]);
    }
    if (!realToMade.empty() || !madeToReal.empty()) { stats(realToMade, "real -> made"); stats(madeToReal, "made -> real"); }
    // the biggest steps: a spike (a frame repeated, a warp) shows here
    std::vector<size_t> order(steps.size()); for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return steps[a] > steps[b]; });
    printf("  the biggest steps (frame number: change, ms since the frame before):");
    for (size_t k = 0; k < std::min<size_t>(5, order.size()); ++k) printf("  %zu: %.2f (%.1f ms)", first + order[k] + 1, steps[order[k]], dts[order[k]]);
    printf("\n");
    // a repeating pulse: the autocorrelation of the steps at lags 2..12
    double mean = 0; for (double x : steps) mean += x; mean /= steps.size();
    double var = 0; for (double x : steps) var += (x - mean) * (x - mean);
    int bestLag = 0; double best = 0;
    for (int lag = 2; lag <= 12 && static_cast<size_t>(lag) < steps.size() / 2; ++lag) {
        double c = 0; for (size_t i = 0; i + lag < steps.size(); ++i) c += (steps[i] - mean) * (steps[i + lag] - mean);
        const double ac = var > 0 ? c / var : 0; if (ac > best) { best = ac; bestLag = lag; }
    }
    if (best > 0.35) printf("  the change repeats every %d frames (autocorrelation %.2f): a pulse of that period, not noise\n", bestLag, best);
    else printf("  no repeating pulse (the best period, %d frames, correlates %.2f)\n", bestLag, best);
    // the time between frames, when it is recorded
    std::vector<double> d2; for (double d : dts) if (d > 0) d2.push_back(d);
    if (d2.size() > 3) stats(d2, "time between frames", "ms");
    return 0;
}

int Video(int argc, char** argv); int VideoCheck(int argc, char** argv); int Unvideo(int argc, char** argv);   // (lsrec_video.cpp)

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "info")) return Info(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "flicker")) return Flicker(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "export")) return Export(argc, argv);
    if (argc >= 6 && !strcmp(argv[1], "make")) return Make(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "video")) return Video(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "videocheck")) return VideoCheck(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "unvideo")) return Unvideo(argc, argv);
    printf("nr_lsrec info <file.lsrec>\nnr_lsrec export <file.lsrec> <folder> [every=N] [first=N] [count=N]\nnr_lsrec make <file.lsrec> <width> <height> <frames> [fps=N] [hdr=1]\nnr_lsrec video <file.lsrec> <out.mkv> [codec=ffv1|x264] [preset=slow] [verify=1]\nnr_lsrec videocheck <file.lsrec> <in.mkv>\nnr_lsrec unvideo <in.mkv> <out.lsrec>\n");
    return 1;
}
