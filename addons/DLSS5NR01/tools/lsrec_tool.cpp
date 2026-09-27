// nr_lsrec: looks into recordings (.lsrec, addon/lsrec.h).
//   nr_lsrec info <file>                                   what is in it: size, format, frames, time, frame rate, frames left out
//   nr_lsrec export <file> <folder> [every=N] [first=N] [count=N]   frames as BMP pictures (HDR ones tone-mapped as the screenshots are)
//   nr_lsrec make <file> <width> <height> <frames> [fps=N]  a made-up recording of a moving picture (for the tests, no game needed)
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
        wchar_t name[32]; swprintf(name, 32, L"\\frame_%05zu.bmp", i);
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
    std::vector<Frame> frames(n);
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (uint32_t t = 0; t < n; ++t) {
        for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
            const double u = x + 0.5 - t * 3.3, v = y + 0.5 - t * 1.4;
            const uint32_t c = Hash(static_cast<uint32_t>(static_cast<int64_t>(std::floor(u / 12 + 100000))), static_cast<uint32_t>(static_cast<int64_t>(std::floor(v / 12 + 100000))));
            uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>(40 + (c & 0x9F)); p[1] = static_cast<uint8_t>(40 + ((c >> 8) & 0x9F)); p[2] = static_cast<uint8_t>(40 + ((c >> 16) & 0x9F)); p[3] = 255;
        }
        frames[t].header.index = t + 1; frames[t].header.qpc = static_cast<int64_t>(t) * 10000000 / fps; frames[t].header.rawBytes = w * h * 4;
        Compress(px.data(), w, h, w * 4, frames[t].data);
    }
    FileHeader header; header.width = w; header.height = h; header.format = 87; header.bytesPerPixel = 4; header.source = kPresented; header.qpcFrequency = 10000000;
    snprintf(header.game, sizeof header.game, "nr_lsrec make");
    std::vector<const Frame*> list; for (const Frame& f : frames) list.push_back(&f);
    std::string error;
    if (!Write(Wide(argv[2]), header, list, &error)) { printf("%s: %s\n", argv[2], error.c_str()); return 3; }
    printf("%u frames of %ux%u at %d frames a second written to %s\n", n, w, h, fps, argv[2]);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "info")) return Info(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "export")) return Export(argc, argv);
    if (argc >= 6 && !strcmp(argv[1], "make")) return Make(argc, argv);
    printf("nr_lsrec info <file.lsrec>\nnr_lsrec export <file.lsrec> <folder> [every=N] [first=N] [count=N]\nnr_lsrec make <file.lsrec> <width> <height> <frames> [fps=N]\n");
    return 1;
}
