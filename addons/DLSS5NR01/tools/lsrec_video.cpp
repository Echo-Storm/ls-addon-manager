// nr_lsrec's video commands: a recording as lossless video (and back), through ffmpeg.
//   nr_lsrec video <file.lsrec> <out.mkv> [codec=ffv1|x264] [preset=slow] [verify=1] [ffmpeg=<path>]   writes the video and <out.mkv>.meta (the file's header and every frame's header)
//   nr_lsrec unvideo <in.mkv> <out.lsrec> [ffmpeg=<path>]                                              the recording again, frame for frame (the pixels are the same; the QOI bytes may differ)
//   nr_lsrec videocheck <file.lsrec> <in.mkv> [ffmpeg=<path>]                                          decodes the video and compares every frame with the recording's
// FFV1 by default (lossless, a frame at a time: on 60 game frames of 2560x1440 it was 16 % of the pictures' size and 2 s, where libx264rgb at QP 0, codec=x264, gave 20 % in 6 s and only takes 8-bit frames
// with nothing in the alpha). Frames in other formats (half floats, 10 bits) go to FFV1 with their
// bytes taken as they are (4 bytes a pixel as bgra or rgba, 8 as rgba64le: bit patterns, so a half float comes back exactly). The picture is the same pixels either way; it is only smaller.
#include "addon/lsrec.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace nr::lsrec;

namespace {

std::wstring Wide(const char* s) {
    std::wstring w(strlen(s), L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), static_cast<int>(w.size()) + 1);
    w.resize(n > 0 ? n - 1 : 0);
    return w;
}
int Arg(int argc, char** argv, int from, const char* key, int fallback) {
    const size_t n = strlen(key);
    for (int i = from; i < argc; ++i) if (!strncmp(argv[i], key, n) && argv[i][n] == '=') return atoi(argv[i] + n + 1);
    return fallback;
}

struct Meta { char magic[8] = { 'L', 'S', 'V', 'I', 'D', '0', '1', 0 }; uint32_t codec = 0, pixFmt = 0, frames = 0; FileHeader header; };   // codec 0 x264rgb, 1 ffv1; pixFmt 0 bgra 1 rgba 2 rgba64le

std::string FfmpegPath(int argc, char** argv, int from) {
    for (int i = from; i < argc; ++i) if (!strncmp(argv[i], "ffmpeg=", 7)) return argv[i] + 7;
    const char* known[] = { "D:\\Applications\\MPV\\ffmpeg.exe" };
    for (const char* k : known) if (GetFileAttributesA(k) != INVALID_FILE_ATTRIBUTES) return k;
    return "ffmpeg.exe";
}
const char* PixName(uint32_t p) { return p == 0 ? "bgra" : p == 1 ? "rgba" : "rgba64le"; }
std::string Quote(const std::string& s) { return "\"" + s + "\""; }

struct Pipe { HANDLE process = nullptr, thread = nullptr, in = nullptr, out = nullptr; };

// Starts ffmpeg with a pipe on its stdin (feed) or on its stdout; its stderr goes to `log`.
bool Spawn(const std::string& cmd, bool feed, const std::wstring& log, Pipe& p) {
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, &sa, 1 << 22)) return false;
    HANDLE logFile = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOA si{ sizeof si }; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = feed ? r : nul; si.hStdOutput = feed ? nul : w; si.hStdError = logFile != INVALID_HANDLE_VALUE ? logFile : nul;
    SetHandleInformation(feed ? w : r, HANDLE_FLAG_INHERIT, 0);
    PROCESS_INFORMATION pi{};
    std::string line = cmd;
    const BOOL ok = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(feed ? r : w);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (logFile != INVALID_HANDLE_VALUE) CloseHandle(logFile);
    if (!ok) { CloseHandle(feed ? w : r); return false; }
    p.process = pi.hProcess; p.thread = pi.hThread;
    if (feed) p.in = w; else p.out = r;
    return true;
}
bool WriteAll(HANDLE h, const void* data, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(data);
    while (n) { DWORD put = 0; if (!WriteFile(h, b, static_cast<DWORD>(std::min<size_t>(n, 1 << 22)), &put, nullptr) || !put) return false; b += put; n -= put; }
    return true;
}
bool ReadAll(HANDLE h, void* data, size_t n) {
    uint8_t* b = static_cast<uint8_t*>(data);
    while (n) { DWORD got = 0; if (!ReadFile(h, b, static_cast<DWORD>(std::min<size_t>(n, 1 << 22)), &got, nullptr) || !got) return false; b += got; n -= got; }
    return true;
}
// Ends the process: after its input was closed (it finishes by itself), or at once (kill).
int Finish(Pipe& p, bool kill = false) {
    if (p.in) { CloseHandle(p.in); p.in = nullptr; }
    if (p.out) { CloseHandle(p.out); p.out = nullptr; }
    if (kill) TerminateProcess(p.process, 1);
    DWORD code = 1;
    WaitForSingleObject(p.process, INFINITE);
    GetExitCodeProcess(p.process, &code);
    CloseHandle(p.process); CloseHandle(p.thread);
    return static_cast<int>(code);
}
double Mb(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a);
    return ((static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow) / 1048576.0;
}

} // namespace

int VideoCheck(int argc, char** argv) {
    Reader r; std::string error;
    if (!r.Open(Wide(argv[2]), &error)) { printf("%s: %s\n", argv[2], error.c_str()); return 2; }
    const FileHeader& h = r.Header();
    Meta m; FILE* f = nullptr;
    if (_wfopen_s(&f, (Wide(argv[3]) + L".meta").c_str(), L"rb") || !f || fread(&m, sizeof m, 1, f) != 1) { printf("no %s.meta beside the video\n", argv[3]); if (f) fclose(f); return 2; }
    fclose(f);
    const std::string cmd = Quote(FfmpegPath(argc, argv, 4)) + " -y -loglevel error -i " + Quote(argv[3]) + " -f rawvideo -pix_fmt " + PixName(m.pixFmt) + " -";
    Pipe p;
    if (!Spawn(cmd, false, Wide(argv[3]) + L".check.log", p)) { printf("could not start ffmpeg\n"); return 4; }
    std::vector<uint8_t> want, got(static_cast<size_t>(h.width) * h.height * h.bytesPerPixel);
    size_t bad = 0, first = 0;
    for (size_t i = 0; i < r.Count(); ++i) {
        const bool readWant = r.Read(i, want);
        if (!readWant || !ReadAll(p.out, got.data(), got.size())) { printf("frame %zu: could not be read from the %s\n", i, readWant ? "video" : "recording"); Finish(p, true); return 5; }
        if (memcmp(want.data(), got.data(), got.size()) != 0) { if (!bad) first = i; ++bad; }
    }
    Finish(p, true);
    if (bad) { printf("VIDEO CHECK FAILED: %zu of %zu frames differ (the first is %zu)\n", bad, r.Count(), first); return 6; }
    printf("video check: all %zu frames are the recording's, bit for bit\n", r.Count());
    return 0;
}

int Video(int argc, char** argv) {
    Reader r; std::string error;
    if (!r.Open(Wide(argv[2]), &error)) { printf("%s: %s\n", argv[2], error.c_str()); return 2; }
    const FileHeader& h = r.Header();
    if (!r.Count()) { printf("no frames\n"); return 2; }
    std::string codecArg = "ffv1", preset = "slow";
    for (int i = 4; i < argc; ++i) { if (!strncmp(argv[i], "codec=", 6)) codecArg = argv[i] + 6; if (!strncmp(argv[i], "preset=", 7)) preset = argv[i] + 7; }
    const bool verify = Arg(argc, argv, 4, "verify", 0) != 0;
    const bool rgba8 = h.bytesPerPixel == 4 && (h.format == 28 || h.format == 29), bgra8 = h.bytesPerPixel == 4 && (h.format == 87 || h.format == 91 || h.format == 88);
    Meta m; m.header = h; m.frames = static_cast<uint32_t>(r.Count());
    m.pixFmt = h.bytesPerPixel == 8 ? 2 : rgba8 ? 1 : 0;
    // x264rgb only for 8-bit unorm frames whose alpha (the first, middle and last frame) is all opaque
    bool x264 = codecArg == "x264" && (rgba8 || bgra8);
    std::vector<uint8_t> px;
    if (x264) {
        for (size_t i : { size_t(0), r.Count() / 2, r.Count() - 1 }) {
            if (!r.Read(i, px)) { printf("frame %zu could not be read\n", i); return 3; }
            for (size_t k = 3; k < px.size(); k += 4) if (px[k] != 255) { x264 = false; break; }
            if (!x264) break;
        }
    }
    m.codec = x264 ? 0 : 1;
    double fps = 60.0;   // from the frames' times when they are there
    if (r.Count() > 2 && h.qpcFrequency) {
        std::vector<double> d;
        for (size_t i = 1; i < r.Count(); ++i) { const double ms = (r.FrameInfo(i).qpc - r.FrameInfo(i - 1).qpc) * 1000.0 / h.qpcFrequency; if (ms > 0.5 && ms < 200) d.push_back(ms); }
        if (!d.empty()) { std::sort(d.begin(), d.end()); fps = 1000.0 / d[d.size() / 2]; }
    }
    char head[512];
    snprintf(head, sizeof head, "%s -y -loglevel error -f rawvideo -pix_fmt %s -s %ux%u -framerate %.4f -i - ", Quote(FfmpegPath(argc, argv, 4)).c_str(), PixName(m.pixFmt), h.width, h.height, fps);
    std::string cmd = head;
    if (x264) cmd += "-c:v libx264rgb -qp 0 -preset " + preset + " -pix_fmt bgr24 ";
    else cmd += std::string("-c:v ffv1 -level 3 -coder 1 -context 1 -g 1 -slices 16 -slicecrc 1 -pix_fmt ") + (m.pixFmt == 2 ? "rgba64le" : m.pixFmt == 1 ? "rgba" : "bgra") + " ";
    cmd += Quote(argv[3]);
    Pipe p;
    if (!Spawn(cmd, true, Wide(argv[3]) + L".log", p)) { printf("could not start ffmpeg\n"); return 4; }
    const ULONGLONG t0 = GetTickCount64();
    for (size_t i = 0; i < r.Count(); ++i) {
        if (!r.Read(i, px)) { printf("frame %zu could not be read\n", i); Finish(p, true); return 3; }
        if (!WriteAll(p.in, px.data(), px.size())) { printf("ffmpeg stopped taking frames at %zu (see %s.log)\n", i, argv[3]); Finish(p, true); return 4; }
    }
    const int code = Finish(p);
    if (code != 0) { printf("ffmpeg ended with %d (see %s.log)\n", code, argv[3]); return 4; }
    {   // the sidecar: what the video cannot hold
        FILE* f = nullptr;
        if (_wfopen_s(&f, (Wide(argv[3]) + L".meta").c_str(), L"wb") || !f) { printf("could not write the .meta\n"); return 3; }
        fwrite(&m, sizeof m, 1, f);
        for (size_t i = 0; i < r.Count(); ++i) fwrite(&r.FrameInfo(i), sizeof(FrameHeader), 1, f);
        fclose(f);
    }
    const double videoMb = Mb(Wide(argv[3])), fileMb = Mb(Wide(argv[2]));
    printf("%zu frames %ux%u -> %s (%s): %.1f MB from %.1f MB (%.0f%%) in %.0f s\n", r.Count(), h.width, h.height, argv[3], x264 ? "x264 RGB lossless" : "FFV1 lossless", videoMb, fileMb,
           100.0 * videoMb / std::max(fileMb, 0.001), (GetTickCount64() - t0) / 1000.0);
    if (verify) return VideoCheck(argc, argv + 0) == 0 ? 0 : 6;
    return 0;
}

int Unvideo(int argc, char** argv) {
    Meta m; FILE* f = nullptr;
    if (_wfopen_s(&f, (Wide(argv[2]) + L".meta").c_str(), L"rb") || !f || fread(&m, sizeof m, 1, f) != 1) { printf("no %s.meta beside the video\n", argv[2]); if (f) fclose(f); return 2; }
    if (memcmp(m.magic, "LSVID01", 7) != 0) { fclose(f); printf("%s.meta is not a recording's\n", argv[2]); return 2; }
    std::vector<FrameHeader> infos(m.frames);
    if (fread(infos.data(), sizeof(FrameHeader), m.frames, f) != m.frames) { fclose(f); printf("the .meta is cut short\n"); return 2; }
    fclose(f);
    const FileHeader& h = m.header;
    Pipe p;
    if (!Spawn(Quote(FfmpegPath(argc, argv, 4)) + " -y -loglevel error -i " + Quote(argv[2]) + " -f rawvideo -pix_fmt " + PixName(m.pixFmt) + " -", false, Wide(argv[3]) + L".log", p)) { printf("could not start ffmpeg\n"); return 4; }
    std::vector<Frame> frames(m.frames);
    std::vector<uint8_t> px(static_cast<size_t>(h.width) * h.height * h.bytesPerPixel);
    for (uint32_t i = 0; i < m.frames; ++i) {
        if (!ReadAll(p.out, px.data(), px.size())) { printf("frame %u could not be decoded\n", i); Finish(p, true); return 5; }
        frames[i].header = infos[i]; frames[i].header.codec = kQoi;
        Compress(px.data(), h.width * (h.bytesPerPixel / 4), h.height, h.width * h.bytesPerPixel, frames[i].data);
        frames[i].header.bytes = static_cast<uint32_t>(frames[i].data.size());
    }
    Finish(p, true);
    std::vector<const Frame*> list;
    for (const Frame& fr : frames) list.push_back(&fr);
    std::string error;
    if (!Write(Wide(argv[3]), h, list, &error)) { printf("%s: %s\n", argv[3], error.c_str()); return 3; }
    printf("%u frames written to %s\n", m.frames, argv[3]);
    return 0;
}
