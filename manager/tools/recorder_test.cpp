// Offline test of the recorder service (src/features/recorder_service.*): frames offered, saved, read back, the note beside the file, the clip, the
// flags the addons read. A real Direct3D 11 device (the card, or WARP); no window, no game, nothing of Lossless Scaling.
//   cmake --build . --config Release --target eam_recordertest
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../src/features/recorder_service.h"
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failed = 0;
static void Check(const char* what, bool ok, const std::string& detail = {}) {
    printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  (", detail.empty() ? "" : (detail + ")").c_str());
    if (!ok) ++g_failed;
}

struct Gpu {
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    bool Make() {
        for (D3D_DRIVER_TYPE type : { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP })
            if (SUCCEEDED(D3D11CreateDevice(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return true;
        return false;
    }
    ID3D11Texture2D* Frame(uint32_t w, uint32_t h, uint8_t shade) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4, shade);
        D3D11_SUBRESOURCE_DATA init{ px.data(), w * 4, 0 };
        ID3D11Texture2D* t = nullptr;
        return SUCCEEDED(dev->CreateTexture2D(&d, &init, &t)) ? t : nullptr;
    }
};

static std::wstring TempDir() {
    wchar_t base[MAX_PATH]; GetTempPathW(MAX_PATH, base);
    std::wstring dir = std::wstring(base) + L"eam_recorder_test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}
static std::wstring FindFile(const std::wstring& dir, const wchar_t* pattern) {
    WIN32_FIND_DATAW f; HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring name = dir + L"\\" + f.cFileName; FindClose(h); return name;
}
static std::string ReadText(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb"); if (!f) return {};
    std::string s; char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n); fclose(f); return s;
}

int main() {
    Gpu gpu;
    if (!gpu.Make()) { printf("no Direct3D 11 device\n"); return 1; }
    const std::wstring dir = TempDir();
    eam::rec::Service svc;
    svc.SetLog([](const char* m) { printf("    [recorder] %s\n", m); });
    svc.SetInfoProvider([] { return std::string("\"manager_version\": \"test\",\n  \"addons\": { \"DLSS5NR01\": { \"enabled\": true } }"); });

    printf("== off\n");
    ID3D11Texture2D* frame = gpu.Frame(256, 256, 10);
    svc.Offer(gpu.ctx, frame, 2, 0, 0, 0, 0);
    Check("flags are 0 while off", svc.Flags() == 0);
    Check("a frame offered while off is dropped", svc.GetStatus().frames == 0);
    Check("nothing to save while off", !svc.Save());

    printf("== on: frames offered by an addon\n");
    eam::rec::Settings s; s.on = true; s.seconds = 5; s.budgetMb = 512; s.folder = dir; s.beep = false; s.note = true; s.saveAfter = 12;
    svc.Configure(s);
    Check("flags say on", svc.Flags() == eam::rec::kFlagOn);
    s.shown = true; svc.Configure(s);
    Check("flags say on and shown", svc.Flags() == (eam::rec::kFlagOn | eam::rec::kFlagShown));
    s.shown = false; svc.Configure(s);
    svc.Offer(gpu.ctx, frame, 1, 0, 0, 0, 0);   // source 1, the captured frame: the service's own, ignored
    Check("an offer of the captured frame is ignored (it is the service's own)", svc.GetStatus().frames == 0);
    for (int i = 0; i < 60 && svc.GetStatus().frames < 12; ++i) {
        ID3D11Texture2D* t = gpu.Frame(256, 256, static_cast<uint8_t>(10 + i));
        svc.Offer(gpu.ctx, t, 2, 0, i % 2 ? 1 : 2, 0, 0);
        t->Release();
        gpu.ctx->Flush();
        Sleep(8);
    }
    for (int i = 0; i < 400 && svc.GetStatus().frames < 12; ++i) {   // the compression threads
        ID3D11Texture2D* t = gpu.Frame(256, 256, 99);
        svc.Offer(gpu.ctx, t, 2, 0, 0, 0, 0); t->Release(); gpu.ctx->Flush(); Sleep(5);
    }
    const eam::rec::Settings now = svc.Current();
    Check("the settings read back", now.on && now.seconds == 5 && now.saveAfter == 12);
    for (int i = 0; i < 600; ++i) {   // the test's own save-after fires inside Offer; the file is written on a thread of its own
        svc.Poll();
        if (!FindFile(dir, L"*.lsrec").empty() && !FindFile(dir, L"*.lsrec.json").empty()) break;
        ID3D11Texture2D* t = gpu.Frame(256, 256, 50); svc.Offer(gpu.ctx, t, 2, 0, 0, 0, 0); t->Release(); gpu.ctx->Flush();
        Sleep(10);
    }
    const std::wstring file = FindFile(dir, L"*.lsrec");
    Check("a .lsrec file was saved", !file.empty());
    if (!file.empty()) {
        nr::lsrec::Reader r; std::string error;
        const bool opened = r.Open(file, &error);
        Check("the file opens", opened, error);
        if (opened) {
            Check("it holds the frames, 256x256 BGRA", r.Count() >= 12 && r.Header().width == 256 && r.Header().height == 256, std::to_string(r.Count()) + " frames");
            Check("recorded as the presented frames", r.Header().source == nr::lsrec::kPresented);
            std::vector<uint8_t> px;
            Check("a frame reads back, its pixels as they were", r.Read(0, px) && px.size() == 256u * 256u * 4u && px[0] >= 10 && px[0] == px[4] && px[0] == px[px.size() - 1]);
        }
        const std::wstring note = FindFile(dir, L"*.lsrec.json");
        const std::string text = ReadText(note);
        Check("a note was written beside it", !text.empty());
        Check("...with the picture, the game and what the manager told it", text.find("\"size\": \"256x256\"") != std::string::npos && text.find("\"game\"") != std::string::npos &&
                                                                             text.find("\"manager_version\": \"test\"") != std::string::npos && text.find("\"source\": \"presented\"") != std::string::npos);
    }
    bool ok = false; const std::string last = svc.LastResult(ok);
    Check("the last result says saved", ok && last.find("Saved") == 0, last);

    printf("== a clip while the recorder is off\n");
    eam::rec::Settings off; off.on = false; off.folder = dir; off.beep = false; off.note = false;
    svc.Configure(off);
    Check("off again: flags 0", svc.Flags() == 0);
    const size_t before = [&] { size_t n = 0; WIN32_FIND_DATAW f; HANDLE h = FindFirstFileW((dir + L"\\*.lsrec").c_str(), &f); if (h != INVALID_HANDLE_VALUE) { do ++n; while (FindNextFileW(h, &f)); FindClose(h); } return n; }();
    svc.Clip();
    Check("a clip is running and the recorder is on for it", svc.Clipping() && (svc.Flags() & eam::rec::kFlagOn) != 0);
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 11500) {
        ID3D11Texture2D* t = gpu.Frame(256, 256, static_cast<uint8_t>(GetTickCount64() / 40)); svc.Offer(gpu.ctx, t, 2, 0, 0, 0, 0); t->Release(); gpu.ctx->Flush();
        svc.Poll(); Sleep(33);
    }
    for (int i = 0; i < 100 && svc.Clipping(); ++i) { svc.Poll(); Sleep(30); }
    Check("the clip ended by itself and the recorder went off", !svc.Clipping() && svc.Flags() == 0);
    Sleep(1500);
    size_t after = 0; { WIN32_FIND_DATAW f; HANDLE h = FindFirstFileW((dir + L"\\*.lsrec").c_str(), &f); if (h != INVALID_HANDLE_VALUE) { do ++after; while (FindNextFileW(h, &f)); FindClose(h); } }
    Check("the clip was saved as a second file", after == before + 1, std::to_string(before) + " -> " + std::to_string(after));

    svc.Shutdown();
    frame->Release();
    gpu.ctx->Release(); gpu.dev->Release();
    printf("\n%s\n", g_failed ? "RECORDER TEST FAILED" : "RECORDER TEST PASSED");
    return g_failed ? 1 : 0;
}
