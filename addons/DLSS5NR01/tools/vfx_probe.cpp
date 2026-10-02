// nr_vfxprobe: NVIDIA's RTX Video Super Resolution (VSR), as the Video Effects SDK exposes it (feature "VideoSuperRes", nvngx_vsr.dll), tried on one frame: how long it takes and how
// close to the true frame it gets, against a plain bicubic upscale (issue #8: "only VSR gives the look I want"). The SDK is the user's own download (NVIDIA's licence; nothing of it is in
// this repository or shipped): external\vfx, with the VideoSuperRes feature installed (features\install_feature.ps1).
//
//   nr_vfxprobe <frame.bmp> [vfx=<SDK folder>] [factor=2] [modes=0,1,2,3,4] [runs=30] [save=<folder>]
//
// The frame (24 or 32 bit BMP, e.g. from `nr_lsrec export`) is the truth. It is shrunk by `factor` (a box filter, 1.5 or 2) to make the input; every mode then upscales that input back to the
// frame's size. Printed per mode: the time of one run on the GPU (a hot card's best of `runs`), the PSNR against the truth, and the picture's detail (mean edge strength) as a share of the truth's.
// With save=, each mode's output is written as a BMP there (and the input and the truth).
#include "nvCVImage.h"
#include "nvVideoEffects.h"
#include "nvVFXVideoSuperRes.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

char* g_nvVFXSDKPath = nullptr;   // (the proxy asks for it: where NVVideoEffects.dll is)

struct Rgba { int w = 0, h = 0; std::vector<unsigned char> px; };   // BGRA, top row first

static bool ReadBmp(const char* path, Rgba& out) {
    FILE* f = nullptr; if (fopen_s(&f, path, "rb") != 0 || !f) return false;
    unsigned char head[54]; if (fread(head, 1, 54, f) != 54 || head[0] != 'B' || head[1] != 'M') { fclose(f); return false; }
    const unsigned off = *reinterpret_cast<unsigned*>(head + 10); const int w = *reinterpret_cast<int*>(head + 18); int h = *reinterpret_cast<int*>(head + 22);
    const int bits = *reinterpret_cast<unsigned short*>(head + 28); const bool topDown = h < 0; if (topDown) h = -h;
    if ((bits != 24 && bits != 32) || w <= 0 || h <= 0) { fclose(f); return false; }
    const int rowBytes = ((w * bits + 31) / 32) * 4; std::vector<unsigned char> raw(static_cast<size_t>(rowBytes) * h);
    fseek(f, static_cast<long>(off), SEEK_SET); const size_t got = fread(raw.data(), 1, raw.size(), f); fclose(f);
    if (got != raw.size()) return false;
    out.w = w; out.h = h; out.px.assign(static_cast<size_t>(w) * h * 4, 255);
    for (int y = 0; y < h; ++y) {
        const unsigned char* src = raw.data() + static_cast<size_t>(topDown ? y : h - 1 - y) * rowBytes;
        unsigned char* dst = out.px.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) { dst[x * 4] = src[x * (bits / 8)]; dst[x * 4 + 1] = src[x * (bits / 8) + 1]; dst[x * 4 + 2] = src[x * (bits / 8) + 2]; }
    }
    return true;
}

static bool WriteBmp(const std::string& path, const Rgba& im) {
    FILE* f = nullptr; if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    const unsigned size = 54 + im.w * im.h * 4; unsigned char head[54] = { 'B', 'M' };
    memcpy(head + 2, &size, 4); const unsigned off = 54; memcpy(head + 10, &off, 4); const unsigned dib = 40; memcpy(head + 14, &dib, 4);
    memcpy(head + 18, &im.w, 4); const int nh = -im.h; memcpy(head + 22, &nh, 4); const unsigned short planes = 1, bits = 32; memcpy(head + 26, &planes, 2); memcpy(head + 28, &bits, 2);
    fwrite(head, 1, 54, f); fwrite(im.px.data(), 1, im.px.size(), f); fclose(f); return true;
}

static Rgba BoxShrink(const Rgba& s, int w, int h) {   // area average to w x h
    Rgba o; o.w = w; o.h = h; o.px.assign(static_cast<size_t>(w) * h * 4, 255);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const int x0 = x * s.w / w, x1 = std::max(x0 + 1, (x + 1) * s.w / w), y0 = y * s.h / h, y1 = std::max(y0 + 1, (y + 1) * s.h / h);
        for (int c = 0; c < 3; ++c) {
            double sum = 0; int n = 0;
            for (int yy = y0; yy < y1 && yy < s.h; ++yy) for (int xx = x0; xx < x1 && xx < s.w; ++xx) { sum += s.px[(static_cast<size_t>(yy) * s.w + xx) * 4 + c]; ++n; }
            o.px[(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<unsigned char>(sum / std::max(1, n) + 0.5);
        }
    }
    return o;
}

static double Psnr(const Rgba& a, const Rgba& b) {
    double se = 0; const size_t n = static_cast<size_t>(a.w) * a.h;
    for (size_t i = 0; i < n; ++i) for (int c = 0; c < 3; ++c) { const double d = double(a.px[i * 4 + c]) - double(b.px[i * 4 + c]); se += d * d; }
    const double mse = se / (3.0 * n); return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

static double Detail(const Rgba& a) {   // mean edge strength of the luma
    double sum = 0; size_t n = 0;
    auto luma = [&](int x, int y) { const unsigned char* p = &a.px[(static_cast<size_t>(y) * a.w + x) * 4]; return 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2]; };
    for (int y = 0; y + 1 < a.h; y += 2) for (int x = 0; x + 1 < a.w; x += 2) { const double l = luma(x, y); sum += std::abs(luma(x + 1, y) - l) + std::abs(luma(x, y + 1) - l); ++n; }
    return n ? sum / n : 0;
}

static const char* Check(NvCV_Status s, const char* what) { if (s == NVCV_SUCCESS) return nullptr; static char t[160]; snprintf(t, sizeof t, "%s failed: %s (%d)", what, NvCV_GetErrorStringFromCode(s), static_cast<int>(s)); return t; }

// ---- the sequence test (seq=<folder of frame_*.bmp>): does the upscaler keep steady from one frame to the next?
// Each frame is the truth: shrunk by `factor` to make the input, upscaled back, and the result compared with the one before. Where the game's own picture did not change (at most 3 levels), the
// result must not change either: that is the shimmer ("still"). Over the whole picture the change of the result should be the change of the truth: its difference is the error of the change ("moving").
struct SeqScore { double still = 0, moving = 0, psnr = 0, detail = 0, ms = 0; int pairs = 0, frames = 0; };

static std::vector<std::string> ListFrames(const std::string& folder) {
    std::vector<std::string> names; WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((folder + "\\frame_*.bmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do names.push_back(folder + "\\" + fd.cFileName); while (FindNextFileA(h, &fd));
    FindClose(h); std::sort(names.begin(), names.end()); return names;
}

static void Pair(const Rgba& truth, const Rgba& truthPrev, const Rgba& out, const Rgba& outPrev, SeqScore& s) {
    double still = 0, moving = 0; size_t nStill = 0; const size_t n = static_cast<size_t>(truth.w) * truth.h;
    for (size_t i = 0; i < n; ++i) {
        int tmax = 0, omax = 0; double merr = 0;
        for (int c = 0; c < 3; ++c) {
            const int dt = int(truth.px[i * 4 + c]) - int(truthPrev.px[i * 4 + c]), dout = int(out.px[i * 4 + c]) - int(outPrev.px[i * 4 + c]);
            tmax = std::max(tmax, std::abs(dt)); omax = std::max(omax, std::abs(dout)); merr += std::abs(dout - dt);
        }
        moving += merr / 3.0;
        if (tmax <= 3) { still += omax; ++nStill; }
    }
    if (nStill > n / 100) { s.still += still / nStill; ++s.pairs; }
    s.moving += moving / n;
}

static int RunSequence(const std::string& folder, const std::string& models, double factor, const std::string& modes, int maxFrames, CUstream stream, const std::string& save) {
    const std::vector<std::string> files = ListFrames(folder);
    if (files.size() < 3) { printf("no frame_*.bmp (at least 3) in %s\n", folder.c_str()); return 2; }
    const int count = std::min<int>(maxFrames, static_cast<int>(files.size()));
    std::vector<int> list; for (size_t pos = 0; pos < modes.size();) { const size_t comma = modes.find(',', pos); list.push_back(atoi(modes.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos).c_str())); pos = comma == std::string::npos ? modes.size() : comma + 1; }
    // the frames are read once and shrunk once
    std::vector<Rgba> truth(count), input(count);
    for (int i = 0; i < count; ++i) { if (!ReadBmp(files[i].c_str(), truth[i])) { printf("cannot read %s\n", files[i].c_str()); return 2; } }
    const int ow = truth[0].w, oh = truth[0].h, iw = std::max(64, static_cast<int>(ow / factor + 0.5)), ih = std::max(64, static_cast<int>(oh / factor + 0.5));
    for (int i = 0; i < count; ++i) input[i] = BoxShrink(truth[i], iw, ih);
    printf("%d frames, truth %dx%d, input %dx%d (factor %.2f)\n", count, ow, oh, iw, ih, factor);
    SeqScore game;   // the game's own shimmer where its picture is still, for scale: how much the truth itself changes there (at most 3 by the definition) is not shown; its detail is the reference
    double truthDetail = 0; for (int i = 0; i < count; ++i) truthDetail += Detail(truth[i]); truthDetail /= count;
    printf("\n%-5s %-14s %9s %9s %9s %8s %9s\n", "mode", "name", "ms/frame", "still", "moving", "PSNR", "detail");
    static const char* names[] = { "VSR_Bicubic", "VSR_Low", "VSR_Medium", "VSR_High", "VSR_Ultra" };
    for (int mode : list) {
        NvVFX_Handle fx = nullptr; NvCVImage srcGpu{}, dstGpu{}, srcCpu{}, dstCpu{};
        auto fail = [&](const char* e) { printf("%-5d %-14s  %s\n", mode, mode >= 0 && mode < 5 ? names[mode] : "?", e); if (fx) NvVFX_DestroyEffect(fx); NvCVImage_Dealloc(&srcGpu); NvCVImage_Dealloc(&dstGpu); };
        if (const char* e = Check(NvVFX_CreateEffect(NVVFX_FX_VIDEO_SUPER_RES, &fx), "create")) { fail(e); continue; }
        NvVFX_SetCudaStream(fx, NVVFX_CUDA_STREAM, stream); NvVFX_SetString(fx, NVVFX_MODEL_DIRECTORY, models.c_str()); NvVFX_SetU32(fx, NVVFX_QUALITY_LEVEL, static_cast<unsigned>(mode));
        if (const char* e = Check(NvCVImage_Alloc(&srcGpu, iw, ih, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1), "alloc input")) { fail(e); continue; }
        if (const char* e = Check(NvCVImage_Alloc(&dstGpu, ow, oh, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1), "alloc output")) { fail(e); continue; }
        NvVFX_SetImage(fx, NVVFX_INPUT_IMAGE, &srcGpu); NvVFX_SetImage(fx, NVVFX_OUTPUT_IMAGE, &dstGpu);
        if (const char* e = Check(NvVFX_Load(fx), "load")) { fail(e); continue; }
        SeqScore sc; Rgba out, outPrev; out.w = ow; out.h = oh; out.px.assign(static_cast<size_t>(ow) * oh * 4, 0); bool ok = true;
        for (int i = 0; i < count && ok; ++i) {
            NvCVImage_Init(&srcCpu, iw, ih, iw * 4, input[i].px.data(), NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
            NvCVImage_Init(&dstCpu, ow, oh, ow * 4, out.px.data(), NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
            if (const char* e = Check(NvCVImage_Transfer(&srcCpu, &srcGpu, 1.0f, stream, nullptr), "upload")) { fail(e); ok = false; break; }
            const auto t0 = std::chrono::steady_clock::now();
            if (const char* e = Check(NvVFX_Run(fx, 0), "run")) { fail(e); ok = false; break; }
            { NvCVImage corner{}, cornerCpu{}; unsigned char pixels[16 * 16 * 4]; NvCVImage_InitView(&corner, &dstGpu, 0, 0, 16, 16);
              NvCVImage_Init(&cornerCpu, 16, 16, 16 * 4, pixels, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU); NvCVImage_Transfer(&corner, &cornerCpu, 1.0f, stream, nullptr); }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (const char* e = Check(NvCVImage_Transfer(&dstGpu, &dstCpu, 1.0f, stream, nullptr), "download")) { fail(e); ok = false; break; }
            if (i >= 2) sc.ms += ms;   // (the first runs warm the card up)
            sc.psnr += Psnr(truth[i], out); sc.detail += Detail(out) / std::max(1e-9, Detail(truth[i])); ++sc.frames;
            if (i > 0) Pair(truth[i], truth[i - 1], out, outPrev, sc);
            if (!save.empty() && (i == count / 2 || i == count / 2 - 1)) WriteBmp(save + "\\seq_mode" + std::to_string(mode) + "_" + std::to_string(i) + ".bmp", out);
            outPrev = out;
        }
        if (!ok) continue;
        const int timed = std::max(1, count - 2);
        printf("%-5d %-14s %9.2f %9.3f %9.3f %7.2f %8.0f %%\n", mode, mode >= 0 && mode < 5 ? names[mode] : "?", sc.ms / timed, sc.pairs ? sc.still / sc.pairs : 0.0, sc.moving / std::max(1, count - 1), sc.psnr / sc.frames, 100.0 * sc.detail / sc.frames);
        NvVFX_DestroyEffect(fx); NvCVImage_Dealloc(&srcGpu); NvCVImage_Dealloc(&dstGpu);
    }
    printf("\nstill  = how much the result changes (levels of 255) where the game's own picture did not (at most 3); lower is steadier\n");
    printf("moving = how far the result's change from the frame before is from the truth's change (levels of 255); lower is better\n");
    (void)truthDetail; (void)game;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("nr_vfxprobe <frame.bmp> [vfx=<SDK folder>] [factor=2] [modes=0,1,2,3,4] [runs=30] [save=<folder>]\nnr_vfxprobe seq=<folder of frame_*.bmp> [vfx=...] [factor=1.5] [modes=0,1,3] [frames=40] [save=<folder>]   (steadiness from frame to frame)\n"); return 2; }
    std::string vfx = "..\\..\\external\\vfx", save, seq, modes = "0,1,2,3,4"; double factor = 2.0; int runs = 30, maxFrames = 40;
    for (int i = 1; i < argc; ++i) {
        if (!strncmp(argv[i], "seq=", 4)) seq = argv[i] + 4; else if (!strncmp(argv[i], "frames=", 7)) maxFrames = std::max(3, atoi(argv[i] + 7));
        else if (!strncmp(argv[i], "vfx=", 4)) vfx = argv[i] + 4; else if (!strncmp(argv[i], "factor=", 7)) factor = atof(argv[i] + 7);
        else if (!strncmp(argv[i], "modes=", 6)) modes = argv[i] + 6; else if (!strncmp(argv[i], "runs=", 5)) runs = std::max(1, atoi(argv[i] + 5)); else if (!strncmp(argv[i], "save=", 5)) save = argv[i] + 5;
    }
    for (char& c : vfx) if (c == '/') c = '\\';
    while (!vfx.empty() && vfx.back() == '\\') vfx.pop_back();
    const std::string bin = vfx + "\\bin", models = vfx + "\\bin\\models";
    for (const char* dll : { "NVCVImage.dll", "NVVideoEffects.dll" }) {   // by full path first, with the DLL's own folder searched for what it needs: the proxies then find them loaded
        if (!LoadLibraryExA((bin + "\\" + dll).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) { printf("cannot load %s\\%s (Windows error %lu: a DLL it needs is missing, or the CUDA 13 driver is not there)\n", bin.c_str(), dll, GetLastError()); return 1; }
    }
    static std::string binStatic; binStatic = bin; g_nvVFXSDKPath = binStatic.data();
    // The proxies load the SDK's DLLs by name: the folders go first on PATH and the DLL directory, and "USE_APP_PATH" keeps them from looking in Program Files.
    _putenv_s("NV_VIDEO_EFFECTS_PATH", "USE_APP_PATH");
    { const char* old = getenv("PATH"); const std::string path = bin + ";" + vfx + "\\features\\nvvfxvideosuperres\\bin;" + (old ? old : ""); _putenv_s("PATH", path.c_str()); }
    SetDllDirectoryA(bin.c_str());
    if (!seq.empty()) {   // the sequence test
        CUstream st = nullptr;
        if (const char* e = Check(NvVFX_CudaStreamCreate(&st), "NvVFX_CudaStreamCreate")) { printf("%s\n", e); return 1; }
        if (!save.empty()) CreateDirectoryA(save.c_str(), nullptr);
        const int r = RunSequence(seq, models, factor, modes, maxFrames, st, save); NvVFX_CudaStreamDestroy(st); return r;
    }
    Rgba truth; if (!ReadBmp(argv[1], truth)) { printf("cannot read %s (a 24 or 32 bit BMP)\n", argv[1]); return 2; }
    const int iw = std::max(64, static_cast<int>(truth.w / factor + 0.5)), ih = std::max(64, static_cast<int>(truth.h / factor + 0.5));
    const Rgba input = BoxShrink(truth, iw, ih);
    printf("truth %dx%d, input %dx%d (factor %.2f), output %dx%d\n", truth.w, truth.h, iw, ih, factor, truth.w, truth.h);
    printf("truth detail %.2f; input detail (at its own size) %.2f\n", Detail(truth), Detail(input));
    if (!save.empty()) { CreateDirectoryA(save.c_str(), nullptr); WriteBmp(save + "\\truth.bmp", truth); WriteBmp(save + "\\input.bmp", input); }

    CUstream stream = nullptr;
    if (const char* e = Check(NvVFX_CudaStreamCreate(&stream), "NvVFX_CudaStreamCreate")) { printf("%s\n(is the SDK in %s, with the CUDA runtime beside it and an NVIDIA card?)\n", e, bin.c_str()); return 1; }

    printf("\n%-5s %-14s %9s %8s %9s\n", "mode", "name", "ms (best)", "PSNR", "detail");
    static const char* names[] = { "VSR_Bicubic", "VSR_Low", "VSR_Medium", "VSR_High", "VSR_Ultra" };
    for (size_t pos = 0; pos < modes.size();) {
        const size_t comma = modes.find(',', pos); const int mode = atoi(modes.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos).c_str());
        pos = comma == std::string::npos ? modes.size() : comma + 1;
        NvVFX_Handle fx = nullptr; NvCVImage srcGpu{}, dstGpu{}, srcCpu{}, dstCpu{};
        auto fail = [&](const char* e) { printf("%-5d %-14s  %s\n", mode, mode >= 0 && mode < 5 ? names[mode] : "?", e); if (fx) NvVFX_DestroyEffect(fx); NvCVImage_Dealloc(&srcGpu); NvCVImage_Dealloc(&dstGpu); };
        if (const char* e = Check(NvVFX_CreateEffect(NVVFX_FX_VIDEO_SUPER_RES, &fx), "NvVFX_CreateEffect(VideoSuperRes)")) { fail(e); continue; }
        NvVFX_SetCudaStream(fx, NVVFX_CUDA_STREAM, stream);
        NvVFX_SetString(fx, NVVFX_MODEL_DIRECTORY, models.c_str());
        NvVFX_SetU32(fx, NVVFX_QUALITY_LEVEL, static_cast<unsigned>(mode));
        if (const char* e = Check(NvCVImage_Alloc(&srcGpu, iw, ih, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1), "alloc input")) { fail(e); continue; }
        if (const char* e = Check(NvCVImage_Alloc(&dstGpu, truth.w, truth.h, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1), "alloc output")) { fail(e); continue; }
        NvCVImage_Init(&srcCpu, iw, ih, iw * 4, const_cast<unsigned char*>(input.px.data()), NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
        Rgba out; out.w = truth.w; out.h = truth.h; out.px.assign(static_cast<size_t>(truth.w) * truth.h * 4, 0);
        NvCVImage_Init(&dstCpu, truth.w, truth.h, truth.w * 4, out.px.data(), NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
        NvVFX_SetImage(fx, NVVFX_INPUT_IMAGE, &srcGpu); NvVFX_SetImage(fx, NVVFX_OUTPUT_IMAGE, &dstGpu);
        if (const char* e = Check(NvVFX_Load(fx), "NvVFX_Load")) { fail(e); continue; }
        if (const char* e = Check(NvCVImage_Transfer(&srcCpu, &srcGpu, 1.0f, stream, nullptr), "upload")) { fail(e); continue; }
        double best = 1e9; bool ok = true;
        for (int r = 0; r < runs + 3; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            if (const char* e = Check(NvVFX_Run(fx, 0), "NvVFX_Run")) { fail(e); ok = false; break; }
            {   // Run may return before the card has finished: a small corner copied back on the same stream waits for it
                NvCVImage corner{}, cornerCpu{}; unsigned char pixels[16 * 16 * 4];
                NvCVImage_InitView(&corner, &dstGpu, 0, 0, 16, 16);
                NvCVImage_Init(&cornerCpu, 16, 16, 16 * 4, pixels, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU);
                NvCVImage_Transfer(&corner, &cornerCpu, 1.0f, stream, nullptr);
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (r >= 3) best = std::min(best, ms);   // (the first runs warm the card and the model up)
        }
        if (!ok) continue;
        if (const char* e = Check(NvCVImage_Transfer(&dstGpu, &dstCpu, 1.0f, stream, nullptr), "download")) { fail(e); continue; }
        printf("%-5d %-14s %9.2f %7.2f %8.0f %%\n", mode, mode >= 0 && mode < 5 ? names[mode] : "?", best, Psnr(truth, out), 100.0 * Detail(out) / std::max(1e-9, Detail(truth)));
        if (!save.empty()) WriteBmp(save + "\\mode" + std::to_string(mode) + ".bmp", out);
        NvVFX_DestroyEffect(fx); NvCVImage_Dealloc(&srcGpu); NvCVImage_Dealloc(&dstGpu);
    }
    NvVFX_CudaStreamDestroy(stream);
    return 0;
}
