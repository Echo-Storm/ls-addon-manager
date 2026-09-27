#include "addon/screenshot.h"
#include "addon/state.h"
#include "addon/log.h"
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

namespace nr::screenshot {

namespace {

std::atomic<bool> g_requested{ false }, g_writing{ false }, g_choosing{ false }, g_snapshotRequested{ false };
struct Snapshot { std::vector<unsigned char> rgba; unsigned w = 0, h = 0; uint64_t serial = 0; };
std::mutex g_snapshotMutex;
Snapshot g_snapshot;
constexpr unsigned kSnapshotWidth = 960;
std::mutex g_resultMutex;
std::string g_result; bool g_resultOk = false;

// The copy in progress (render thread only, under g_frameMutex).
struct Pending { ID3D11Texture2D* staging = nullptr; DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; unsigned w = 0, h = 0; int presentsWaited = 0; std::wstring path; bool snapshot = false; };
Pending g_pending;
constexpr int kWaitPresents = 30;   // after this many presents the read-back waits for the GPU (it has long finished by then)

void SetResult(const std::string& text, bool ok) {
    std::lock_guard<std::mutex> lock(g_resultMutex);
    g_result = text; g_resultOk = ok;
}

std::wstring Wide(const std::string& s) {
    std::wstring w(s.size(), L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    w.resize(n > 0 ? n : 0);
    return w;
}
std::string Utf8(const std::wstring& w) {
    std::string s(w.size() * 3, '\0');
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), static_cast<int>(s.size()), nullptr, nullptr);
    s.resize(n > 0 ? n : 0);
    return s;
}

// "WowB_2026-09-23_14-05-33.png" in the folder, made if it does not exist.
std::wstring NewPath(const std::string& game) {
    const std::wstring folder = Folder();
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    std::wstring name = Wide(game.empty() ? std::string("Lossless Scaling") : game);
    if (const size_t dot = name.rfind(L'.'); dot != std::wstring::npos && dot > 0) name.resize(dot);
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t stamp[40];
    swprintf(stamp, 40, L"_%04u-%02u-%02u_%02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring path = folder + L"\\" + name + stamp;
    std::wstring candidate = path + L".png";
    for (int n = 2; GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES && n < 100; ++n) candidate = path + L"-" + std::to_wstring(n) + L".png";
    return candidate;
}

bool WritePng(const std::wstring& path, const std::vector<unsigned char>& bgra, unsigned w, unsigned h, std::string& error) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr; IWICStream* stream = nullptr; IWICBitmapEncoder* encoder = nullptr; IWICBitmapFrameEncode* frame = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(w, h);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr) && format != GUID_WICPixelFormat32bppBGRA) hr = E_UNEXPECTED;
    if (SUCCEEDED(hr)) hr = frame->WritePixels(h, w * 4, static_cast<UINT>(bgra.size()), const_cast<BYTE*>(bgra.data()));
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    for (IUnknown* p : { static_cast<IUnknown*>(frame), static_cast<IUnknown*>(encoder), static_cast<IUnknown*>(stream), static_cast<IUnknown*>(factory) }) if (p) p->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    if (FAILED(hr)) { char text[64]; snprintf(text, sizeof text, "the PNG could not be written (0x%08lx)", static_cast<unsigned long>(hr)); error = text; DeleteFileW(path.c_str()); }
    return SUCCEEDED(hr);
}

// The copy has reached the CPU: convert it and write it on a thread of its own.
void Finish(ID3D11DeviceContext* ctx, bool wait) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = ctx->Map(g_pending.staging, 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
    Pending done = g_pending;
    g_pending = {};
    if (FAILED(hr)) { done.staging->Release(); SetResult("The picture could not be read back from the graphics card.", false); g_writing = false; return; }
    std::vector<unsigned char> bgra(static_cast<size_t>(done.w) * done.h * 4);
    bool converted = true;
    for (unsigned y = 0; y < done.h && converted; ++y)
        converted = ToBgra8(done.format, static_cast<const unsigned char*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, done.w, bgra.data() + static_cast<size_t>(y) * done.w * 4);
    ctx->Unmap(done.staging, 0);
    done.staging->Release();
    if (!converted) { SetResult("Frames in this format cannot be saved yet.", false); g_writing = false; return; }
    if (done.snapshot) {   // shrunk by a whole factor with a box filter, and turned to RGBA for the panel
        const unsigned factor = (done.w + kSnapshotWidth - 1) / kSnapshotWidth, sw = done.w / factor, sh = done.h / factor;
        Snapshot shot; shot.w = sw; shot.h = sh; shot.rgba.resize(static_cast<size_t>(sw) * sh * 4);
        for (unsigned y = 0; y < sh; ++y)
            for (unsigned x = 0; x < sw; ++x) {
                unsigned sum[3] = {};
                for (unsigned dy = 0; dy < factor; ++dy)
                    for (unsigned dx = 0; dx < factor; ++dx) {
                        const unsigned char* p = bgra.data() + ((static_cast<size_t>(y) * factor + dy) * done.w + static_cast<size_t>(x) * factor + dx) * 4;
                        sum[0] += p[2]; sum[1] += p[1]; sum[2] += p[0];
                    }
                unsigned char* out = shot.rgba.data() + (static_cast<size_t>(y) * sw + x) * 4;
                for (int k = 0; k < 3; ++k) out[k] = static_cast<unsigned char>(sum[k] / (factor * factor));
                out[3] = 255;
            }
        { std::lock_guard<std::mutex> lock(g_snapshotMutex); shot.serial = g_snapshot.serial + 1; g_snapshot = std::move(shot); }
        g_writing = false;
        return;
    }
    std::thread([pixels = std::move(bgra), done] {
        std::string error;
        if (WritePng(done.path, pixels, done.w, done.h, error)) { SetResult("Saved " + Utf8(done.path), true); Log("screenshot: %s", Utf8(done.path).c_str()); }
        else { SetResult("Not saved: " + error, false); Log("screenshot failed: %s", error.c_str()); }
        g_writing = false;
    }).detach();
}

} // namespace

// ---- pictures taken now (Capture): their own list, apart from the present's screenshot
namespace {
struct Captured { ID3D11Texture2D* staging = nullptr; DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; unsigned w = 0, h = 0; int ticks = 0; std::wstring path;
                  uint32_t encoding = 0; float white = 200.0f; };
std::vector<Captured> g_captures;   // render thread only (under g_frameMutex)
}

std::wstring PairBase(const std::string& game) {
    std::wstring path = NewPath(game);   // "...\<game>_<date>.png", free
    if (path.size() > 4) path.resize(path.size() - 4);
    return path;
}

bool Capture(ID3D11DeviceContext* ctx, ID3D11Resource* source, const D3D11_BOX& region, DXGI_FORMAT viewFormat, const std::wstring& path,
             uint32_t encoding, float white) {
    if (!ctx || !source || region.right <= region.left || region.bottom <= region.top) return false;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(source->QueryInterface(IID_PPV_ARGS(&tex)))) return false;
    D3D11_TEXTURE2D_DESC desc; tex->GetDesc(&desc); tex->Release();
    if (desc.SampleDesc.Count != 1) return false;
    D3D11_TEXTURE2D_DESC s{}; s.Width = region.right - region.left; s.Height = region.bottom - region.top; s.MipLevels = 1; s.ArraySize = 1;
    s.Format = desc.Format; s.SampleDesc = { 1, 0 }; s.Usage = D3D11_USAGE_STAGING; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    ID3D11Texture2D* staging = nullptr;
    const bool made = dev && SUCCEEDED(dev->CreateTexture2D(&s, nullptr, &staging));
    if (dev) dev->Release();
    if (!made) { SetResult("The picture could not be copied.", false); return false; }
    D3D11_BOX box = region; box.front = 0; box.back = 1;
    ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, source, 0, &box);
    g_captures.push_back({ staging, viewFormat, s.Width, s.Height, 0, path, encoding, white });
    return true;
}

void Tick(ID3D11DeviceContext* ctx) {
    for (size_t i = 0; i < g_captures.size();) {
        Captured& c = g_captures[i];
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = ctx->Map(c.staging, 0, D3D11_MAP_READ, ++c.ticks >= kWaitPresents ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { ++i; continue; }
        const Captured done = c;
        g_captures.erase(g_captures.begin() + static_cast<std::ptrdiff_t>(i));
        if (FAILED(hr)) { done.staging->Release(); SetResult("The picture could not be read back from the graphics card.", false); continue; }
        const size_t rowBytes = static_cast<size_t>(done.w) * (done.format == DXGI_FORMAT_R16G16B16A16_FLOAT || done.format == DXGI_FORMAT_R16G16B16A16_TYPELESS ? 8 : 4);
        std::vector<unsigned char> raw(rowBytes * done.h);
        for (unsigned y = 0; y < done.h; ++y) memcpy(raw.data() + y * rowBytes, static_cast<const unsigned char*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
        ctx->Unmap(done.staging, 0);
        done.staging->Release();
        g_writing = true;
        std::thread([raw = std::move(raw), done, rowBytes] {   // the conversion (HDR: its SDR view) and the PNG, off the render thread
            std::vector<unsigned char> bgra(static_cast<size_t>(done.w) * done.h * 4);
            bool converted = true;
            for (unsigned y = 0; y < done.h && converted; ++y)
                converted = ToBgra8Sdr(done.format, raw.data() + y * rowBytes, done.w, done.encoding, done.white, bgra.data() + static_cast<size_t>(y) * done.w * 4);
            std::string error;
            if (!converted) { SetResult("Frames in this format cannot be saved yet.", false); Log("screenshot: format %d cannot be saved", (int)done.format); }
            else if (WritePng(done.path, bgra, done.w, done.h, error)) { SetResult("Saved " + Utf8(done.path), true); Log("screenshot: %s", Utf8(done.path).c_str()); }
            else { SetResult("Not saved: " + error, false); Log("screenshot failed: %s", error.c_str()); }
            g_writing = false;
        }).detach();
    }
}

void ForgetCaptures() {
    for (Captured& c : g_captures) if (c.staging) c.staging->Release();
    g_captures.clear();
}

void Request() { g_requested = true; }
void RequestSnapshot() { g_snapshotRequested = true; }

bool NewSnapshot(uint64_t& serial, std::vector<unsigned char>& rgba, unsigned& w, unsigned& h) {
    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    if (g_snapshot.serial <= serial || g_snapshot.rgba.empty()) return false;
    serial = g_snapshot.serial; rgba = g_snapshot.rgba; w = g_snapshot.w; h = g_snapshot.h;
    return true;
}

void OnPresent(ID3D11DeviceContext* ctx, IDXGISwapChain* chain, const std::string& game) {
    if (g_pending.staging) Finish(ctx, ++g_pending.presentsWaited >= kWaitPresents);
    if ((!g_requested && !g_snapshotRequested) || g_pending.staging || g_writing) return;
    const bool snapshot = !g_requested && g_snapshotRequested;   // a screenshot first when both were asked for
    (snapshot ? g_snapshotRequested : g_requested) = false;
    ID3D11Texture2D* buffer = nullptr;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&buffer)))) { SetResult("Lossless Scaling's picture could not be reached.", false); return; }
    D3D11_TEXTURE2D_DESC desc; buffer->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.MipLevels = 1; staging.ArraySize = 1; staging.SampleDesc = { 1, 0 }; staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0; staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ; staging.MiscFlags = 0;
    ID3D11Device* dev = nullptr; buffer->GetDevice(&dev);
    ID3D11Texture2D* copy = nullptr;
    const bool made = desc.SampleDesc.Count == 1 && dev && SUCCEEDED(dev->CreateTexture2D(&staging, nullptr, &copy));
    if (dev) dev->Release();
    if (!made) { buffer->Release(); SetResult("This kind of swap chain buffer cannot be copied.", false); return; }
    ctx->CopyResource(copy, buffer);
    buffer->Release();
    g_writing = true;
    g_pending = { copy, desc.Format, desc.Width, desc.Height, 0, snapshot ? std::wstring() : NewPath(game), snapshot };
}

void Forget() {
    ForgetCaptures();
    if (g_pending.staging) { g_pending.staging->Release(); g_pending = {}; g_writing = false; }
}

std::wstring Folder() {
    std::string chosen;
    { std::lock_guard<std::mutex> lock(g_settingsMutex); chosen = g_config.screenshotFolder; }
    if (!chosen.empty()) return Wide(chosen);
    PWSTR pictures = nullptr;
    std::wstring folder = L"C:\\Screenshots";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures))) folder = std::wstring(pictures) + L"\\Lossless Scaling";
    CoTaskMemFree(pictures);
    return folder;
}

void ChooseFolder() {
    if (g_choosing.exchange(true)) return;
    std::thread([] {
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
            IFileOpenDialog* dialog = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
                DWORD options = 0; dialog->GetOptions(&options);
                dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                dialog->SetTitle(L"Where screenshots go");
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->Show(FindWindowW(L"LSAddonManagerClass", nullptr))) && SUCCEEDED(dialog->GetResult(&item))) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        Config c; std::vector<Look> looks;
                        { std::lock_guard<std::mutex> lock(g_settingsMutex); g_config.screenshotFolder = Utf8(path); c = g_config; looks = g_looks; }
                        SaveSettings(g_host, kAddonId, c, looks);
                        Log("screenshots go to %s", Utf8(path).c_str());
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
                dialog->Release();
            }
            CoUninitialize();
        }
        g_choosing = false;
    }).detach();
}

bool Choosing() { return g_choosing; }
bool Busy() { return g_requested || g_snapshotRequested || g_writing; }

std::string LastResult(bool& ok) {
    std::lock_guard<std::mutex> lock(g_resultMutex);
    ok = g_resultOk;
    return g_result;
}

} // namespace nr::screenshot
