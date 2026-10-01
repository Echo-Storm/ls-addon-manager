// nr_selftest — asks one question: does this model file work on this graphics card? It runs in its own process, so whatever the model does (including
// crashing) cannot take Lossless Scaling down with it. The Neural Rendering addon starts it from the Requirements section and reads the last line.
//
//   nr_selftest.exe --model <path to nvngx_dlssnr.dll> [--lsdir <Lossless Scaling folder>] [--luid <high>:<low>] [--size WxH] [--report <file>]
//
// --report writes a short text file to share (card, driver, Windows, the model file's name, version and size, the result, and a line for the table in
// docs/model-compatibility.md): no folders, no user name, no hash. It is written whatever the result is.
//
// It follows the same path as the engine: a Direct3D 12 device on the NVIDIA card, the driver's NGX core, the
// helper DLL (nvngx.dll_dlss5nr01.dll beside this program) which is the only caller the model accepts, the model's own initialisation, creating
// feature 18 at a small size, and evaluating it on a synthetic picture. It passes when the model created its feature and changed the picture.
//
// The last line printed is:  SELFTEST <code> <KEY> <words>   and the exit code is <code>.
//    0 PASS               the model works on this graphics card
//   10 NO_GPU             no NVIDIA graphics card
//   11 D3D12              a Direct3D 12 device could not be created on it
//   12 NGX_CORE           the NVIDIA driver's NGX core did not start
//   13 HELPER             the helper DLL is missing or is from another version of this addon
//   14 MODEL_LOAD         the model file is missing or is not a usable model
//   15 FLOAT_SLOT         the model's parameter block did not behave as expected
//   16 MODEL_INIT         the model loaded but refused to start
//   17 NOT_SUPPORTED      the model created no feature: this build does not support this graphics card
//   18 FEATURE            the model failed to create its feature for another reason
//   19 EVALUATE           the model failed when it ran
//   20 UNCHANGED          the model ran but did not change the picture
#include <windows.h>
#include <bcrypt.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include "engine/ngx_paths.h"
#include "nvsdk_ngx.h"
#include "forwarder/nr_api.h"

namespace {

void Say(const char* fmt, ...) {
    char b[2048];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    fputs(b, stdout); fputc('\n', stdout); fflush(stdout);
}

// What the report says about this machine, filled in as it becomes known (Finish can be reached from any step)
struct ReportInfo {
    std::wstring path;                 // --report <file>: where to write it; empty: no report
    std::string gpu, memory, driver, model, modelSize, modelVersion, size, modelHash;
    std::string adapters;              // every graphics adapter the system has, and which one was tested
    std::string variants;              // when the model refused: what it did at other sizes, so a size-dependent refusal shows
    std::vector<std::string> ngx;      // what NVIDIA's NGX said while the test ran (folders left out), for the report of a failure
} g_report;

// The first 16 hex digits of the file's SHA-256 (it says whether two reports are of the same build of the model, which a name and a size do not); empty when it cannot be made.
std::string FileHash(const std::wstring& path) {
    std::string out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20); DWORD n = 0; bool ok = true;
        while (ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n) if (BCryptHashData(h, buf.data(), n, 0) != 0) { ok = false; break; }
        unsigned char digest[32] = {};
        if (ok && BCryptFinishHash(h, digest, sizeof digest, 0) == 0) { char hex[40]; for (int i = 0; i < 8; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]); out = hex; }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

std::string OsVersion() {
    typedef LONG(WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);
    OSVERSIONINFOW v = {}; v.dwOSVersionInfoSize = sizeof v;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn fn = nt ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(nt, "RtlGetVersion")) : nullptr;
    char b[64] = "unknown";
    if (fn && fn(&v) == 0) snprintf(b, sizeof b, "%lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    return b;
}

// A text file a person can paste into an issue: which card, driver, Windows and model file (its name, version and size only: no folders, no user name, no hash) and what the test said.
void WriteReport(int code, const char* key, const char* words) {
    if (g_report.path.empty()) return;
    SYSTEMTIME t; GetLocalTime(&t);
    char date[32]; snprintf(date, sizeof date, "%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
    const std::string card = g_report.gpu.empty() ? std::string("none found") : g_report.gpu;
    const std::string driver = g_report.driver.empty() ? std::string("unknown") : g_report.driver;
    const std::string model = g_report.model.empty() ? std::string("nvngx_dlssnr.dll") : g_report.model;
    const std::string modelDesc = g_report.modelVersion.empty() && g_report.modelSize.empty() ? std::string("not found")
        : (g_report.modelVersion.empty() ? std::string("unknown version") : "version " + g_report.modelVersion) + (g_report.modelSize.empty() ? "" : ", " + g_report.modelSize);
    std::string row = "| " + card + (g_report.memory.empty() ? "" : " (" + g_report.memory + ")") + " | " + driver + " | " +
        (g_report.modelVersion.empty() ? std::string("unknown") : g_report.modelVersion) + (g_report.modelSize.empty() ? "" : " (" + g_report.modelSize + ")") + " | " +
        (code == 0 ? "PASS" : "FAIL " + std::string(key)) + " | " + NR_ADDON_VERSION_TEXT + " |";
    char head[256];
    snprintf(head, sizeof head, "result:          %s (code %d)\r\n", code == 0 ? "PASS" : key, code);
    std::string text = "Neural Rendering compatibility report\r\n=====================================\r\n";
    text += head;
    text += std::string("what it said:    ") + words + "\r\n";
    text += "graphics card:   " + card + (g_report.memory.empty() ? "" : " (" + g_report.memory + ")") + "\r\n";
    text += "NVIDIA driver:   " + driver + "\r\n";
    text += "Windows:         " + OsVersion() + "\r\n";
    text += "model file:      " + model + ", " + modelDesc + (g_report.modelHash.empty() ? "" : ", sha256 " + g_report.modelHash + "...") + "\r\n";
    if (!g_report.size.empty()) text += "test picture:    " + g_report.size + "\r\n";
    text += std::string("addon:           ") + NR_ADDON_VERSION_TEXT + " (nr_selftest)\r\n";
    text += std::string("date:            ") + date + "\r\n\r\n";
    if (!g_report.adapters.empty()) text += "graphics adapters:\r\n" + g_report.adapters + "\r\n";
    if (!g_report.variants.empty()) text += "the same test at other sizes (the model refused at the size above):\r\n" + g_report.variants + "\r\n";
    if (code != 0 && !g_report.ngx.empty()) {   // what NVIDIA's own code said (the last lines): where a refusal comes from
        text += "what NGX said (the last 40 lines, folders left out):\r\n";
        for (size_t i = g_report.ngx.size() > 40 ? g_report.ngx.size() - 40 : 0; i < g_report.ngx.size(); ++i) text += "  " + g_report.ngx[i] + "\r\n";
        text += "\r\n";
    }
    text += "For the table in docs/model-compatibility.md:\r\n| Card | Driver | Model version | Result | Addon |\r\n|---|---|---|---|---|\r\n" + row + "\r\n";
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_report.path.c_str(), L"wb") == 0 && f) { fwrite(text.data(), 1, text.size(), f); fclose(f); }
}

int Finish(int code, const char* key, const char* words) {
    WriteReport(code, key, words);
    Say("SELFTEST %d %s %s", code, key, words);
    return code;
}

// A line of NGX's with the folders taken out (a drive and a path: a user name can be in it), for a report that goes into a public issue: "C:\Users\x\folder\file.dll" becomes "<folder>\file.dll".
std::string WithoutFolders(const std::string& s) {
    std::string out; size_t i = 0;
    while (i < s.size()) {
        if (i + 2 < s.size() && isalpha(static_cast<unsigned char>(s[i])) && s[i + 1] == ':' && (s[i + 2] == '\\' || s[i + 2] == '/')) {
            size_t j = i; while (j < s.size() && s[j] != ' ' && s[j] != '\'' && s[j] != '"' && s[j] != ')' && s[j] != ',') ++j;
            const size_t slash = s.find_last_of("\\/", j - 1);
            out += "<folder>";
            if (slash != std::string::npos && slash >= i && slash + 1 < j) out += "\\" + s.substr(slash + 1, j - slash - 1);
            i = j;
        } else out += s[i++];
    }
    for (char& c : out) if (c == '\\') c = '/';   // (one separator, and none of the backslashes a path is told by)
    return out;
}

void NVSDK_CONV NgxLog(const char* msg, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::string s(msg ? msg : "");
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (s.find("NGXLoadConfig") == std::string::npos) { Say("  [ngx] %s", s.c_str()); if (g_report.ngx.size() < 400) g_report.ngx.push_back(WithoutFolders(s)); }
}

const char* Name(int r) {
    switch (static_cast<NVSDK_NGX_Result>(r)) {
    case NVSDK_NGX_Result_Success: return "Success";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "FeatureNotSupported";
    case NVSDK_NGX_Result_FAIL_PlatformError: return "PlatformError";
    case NVSDK_NGX_Result_FAIL_InvalidParameter: return "InvalidParameter";
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: return "OutOfGPUMemory";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "UnableToInitializeFeature";
    default: return "failed";
    }
}

// ---- Direct3D 12 in the smallest form that does the job
struct Gpu {
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    UINT64 value = 0;
    std::vector<ID3D12Resource*> garbage;   // upload buffers, released after the next wait

    bool Create(IDXGIAdapter1* adapter) {
        if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return false;
        D3D12_COMMAND_QUEUE_DESC q = {};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)))) return false;
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return false;
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)))) return false;
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return event != nullptr;
    }
    void Run() {   // execute what was recorded and wait for it
        list->Close();
        ID3D12CommandList* lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, ++value);
        if (fence->GetCompletedValue() < value) { fence->SetEventOnCompletion(value, event); WaitForSingleObject(event, 60000); }
        for (auto* g : garbage) g->Release();
        garbage.clear();
        alloc->Reset();
        list->Reset(alloc, nullptr);
    }
};

void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    l->ResourceBarrier(1, &b);
}

ID3D12Resource* Texture(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = flags;
    ID3D12Resource* r = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
    return r;
}

ID3D12Resource* Buffer(ID3D12Device* dev, UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
    return r;
}

// Records a copy of `data` into a texture that is in COPY_DEST, then moves it to `after`.
void Upload(Gpu& g, ID3D12Resource* tex, UINT w, UINT h, UINT bytesPerPixel, const void* data, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_DESC d = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    g.dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowSize, &total);
    ID3D12Resource* up = Buffer(g.dev, total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* dst = nullptr;
    up->Map(0, nullptr, reinterpret_cast<void**>(&dst));
    for (UINT y = 0; y < h; ++y) memcpy(dst + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch, static_cast<const uint8_t*>(data) + static_cast<size_t>(y) * w * bytesPerPixel, static_cast<size_t>(w) * bytesPerPixel);
    up->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
    to.pResource = tex; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = up; from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = fp;
    g.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Barrier(g.list, tex, D3D12_RESOURCE_STATE_COPY_DEST, after);
    g.garbage.push_back(up);
}

std::vector<uint8_t> Readback(Gpu& g, ID3D12Resource* tex, UINT w, UINT h, UINT bytesPerPixel, D3D12_RESOURCE_STATES current) {
    D3D12_RESOURCE_DESC d = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows = 0; UINT64 rowSize = 0, total = 0;
    g.dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowSize, &total);
    ID3D12Resource* rb = Buffer(g.dev, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(g.list, tex, current, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
    from.pResource = tex; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = rb; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = fp;
    g.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Barrier(g.list, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, current);
    g.Run();
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * bytesPerPixel);
    uint8_t* src = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&src));
    for (UINT y = 0; y < h; ++y) memcpy(&out[static_cast<size_t>(y) * w * bytesPerPixel], src + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch, static_cast<size_t>(w) * bytesPerPixel);
    rb->Unmap(0, nullptr);
    rb->Release();
    return out;
}

// ---- the helper DLL, and the driver core's parameter block
struct Helper {
    HMODULE module = nullptr;
    PFN_nrfwd_probe probe = nullptr; PFN_nrfwd_init init = nullptr; PFN_nrfwd_set_float_slot setFloatSlot = nullptr;
    PFN_nrfwd_probe_float probeFloat = nullptr; PFN_nrfwd_get_float getFloat = nullptr; PFN_nrfwd_create create = nullptr;
    PFN_nrfwd_evaluate evaluate = nullptr; PFN_nrfwd_release release = nullptr; PFN_nrfwd_last_result last = nullptr;
    bool Load(const std::wstring& path) {
        module = LoadLibraryW(path.c_str());
        if (!module) return false;
#define BIND(name, var) var = reinterpret_cast<decltype(var)>(GetProcAddress(module, name)); if (!var) return false;
        BIND("nrfwd_probe", probe) BIND("nrfwd_init", init) BIND("nrfwd_set_float_slot", setFloatSlot) BIND("nrfwd_probe_float", probeFloat)
        BIND("nrfwd_get_float", getFloat) BIND("nrfwd_create", create) BIND("nrfwd_evaluate", evaluate) BIND("nrfwd_release", release) BIND("nrfwd_last_result", last)
#undef BIND
        return true;
    }
};

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The model file's version as its resource says it ("310.8" for 310.8.0.0), without loading the file; empty when it has none.
std::string ModelVersionText(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return std::string();
    std::vector<char> block(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, block.data())) return std::string();
    VS_FIXEDFILEINFO* fixedInfo = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&fixedInfo), &len) || !fixedInfo || len < sizeof(VS_FIXEDFILEINFO)) return std::string();
    unsigned p[4] = { HIWORD(fixedInfo->dwFileVersionMS), LOWORD(fixedInfo->dwFileVersionMS), HIWORD(fixedInfo->dwFileVersionLS), LOWORD(fixedInfo->dwFileVersionLS) };
    int n = 4;
    while (n > 2 && p[n - 1] == 0) --n;
    char b[48] = {};
    if (n == 2) snprintf(b, sizeof b, "%u.%u", p[0], p[1]);
    else if (n == 3) snprintf(b, sizeof b, "%u.%u.%u", p[0], p[1], p[2]);
    else snprintf(b, sizeof b, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return b;
}

std::wstring ArgAfter(int argc, wchar_t** argv, const wchar_t* flag) {
    for (int i = 1; i + 1 < argc; ++i) if (std::wstring(argv[i]) == flag) return argv[i + 1];
    return std::wstring();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring exeDir = exe;
    exeDir.resize(exeDir.find_last_of(L'\\'));

    std::wstring model = ArgAfter(argc, argv, L"--model");
    std::wstring lsDir = ArgAfter(argc, argv, L"--lsdir");
    if (lsDir.empty()) {   // the addon folder is <Lossless Scaling>\addons\DLSS5NR01
        lsDir = exeDir;
        for (int up = 0; up < 2 && lsDir.find_last_of(L'\\') != std::wstring::npos; ++up) lsDir.resize(lsDir.find_last_of(L'\\'));
    }
    if (model.empty()) model = lsDir + L"\\nvngx_dlssnr.dll";
    UINT width = 1280, height = 720;
    {
        const std::wstring size = ArgAfter(argc, argv, L"--size");
        UINT w = 0, h = 0;
        if (!size.empty() && swscanf(size.c_str(), L"%ux%u", &w, &h) == 2) { width = w; height = h; }
    }
    if (width < 128 || height < 72 || width > 3840 || height > 2160) { width = 1280; height = 720; }
    const std::wstring luidArg = ArgAfter(argc, argv, L"--luid");

    // the shareable report: only the model file's name, version and size (learned without loading it) go in, never a folder
    g_report.path = ArgAfter(argc, argv, L"--report");
    {
        char sz[32]; snprintf(sz, sizeof sz, "%ux%u", width, height); g_report.size = sz;
        const size_t slash = model.find_last_of(L"\\/");
        g_report.model = Narrow(slash == std::wstring::npos ? model : model.substr(slash + 1));
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        if (GetFileAttributesExW(model.c_str(), GetFileExInfoStandard, &fa)) {
            const double mb = ((static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow) / (1024.0 * 1024.0);
            char m[32]; snprintf(m, sizeof m, "%.1f MB", mb); g_report.modelSize = m;
            g_report.modelVersion = ModelVersionText(model);
            g_report.modelHash = FileHash(model);
        }
    }

    // NGX and the model write their own files under the data path: keep them out of the Lossless Scaling folder
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dataDir = std::wstring(tmp) + L"DLSS5NR01_selftest";
    CreateDirectoryW(dataDir.c_str(), nullptr);

    // ---- the graphics card: the one named by --luid, else the first NVIDIA card
    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    IDXGIAdapter1* adapter = nullptr;
    DXGI_ADAPTER_DESC1 desc = {};
    if (factory) {
        for (UINT i = 0;; ++i) {
            IDXGIAdapter1* a = nullptr;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 d = {};
            a->GetDesc1(&d);
            bool wanted = d.VendorId == 0x10DE && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
            unsigned hi = 0, lo = 0;
            if (wanted && !luidArg.empty() && swscanf(luidArg.c_str(), L"%x:%x", &hi, &lo) == 2)
                wanted = static_cast<unsigned>(d.AdapterLuid.HighPart) == hi && d.AdapterLuid.LowPart == lo;
            {   // every adapter, for the report: a card that is not the one driving the display, a second NVIDIA card, an integrated one
                char n[128] = {}; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, n, sizeof n - 1, nullptr, nullptr);
                char line[256]; snprintf(line, sizeof line, "  %s%s (vendor %04x, device %04x, %llu MB)%s\r\n", n, (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " [software]" : "", d.VendorId, d.DeviceId,
                                         static_cast<unsigned long long>(d.DedicatedVideoMemory >> 20), wanted && !adapter ? "  <- tested" : "");
                g_report.adapters += line;
            }
            if (wanted && !adapter) { adapter = a; desc = d; } else a->Release();
        }
    }
    if (!adapter) return Finish(10, "NO_GPU", "no NVIDIA graphics card was found");
    char gpuName[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpuName, sizeof gpuName - 1, nullptr, nullptr);
    Say("graphics card: %s", gpuName);
    g_report.gpu = gpuName;
    { char m[32]; snprintf(m, sizeof m, "%llu GB", static_cast<unsigned long long>((desc.DedicatedVideoMemory + (512ull << 20)) >> 30)); g_report.memory = m; }
    {   // the driver's number as NVIDIA writes it: the user-mode driver version 32.0.16.1692 is driver 616.92
        LARGE_INTEGER umd = {};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            const unsigned low = static_cast<unsigned>(umd.LowPart);
            const unsigned five = (HIWORD(low) % 10) * 10000 + LOWORD(low);
            char d[32]; snprintf(d, sizeof d, "%u.%02u", five / 100, five % 100); g_report.driver = d;
        }
    }

    Gpu gpu;
    if (!gpu.Create(adapter)) return Finish(11, "D3D12", "a Direct3D 12 device could not be created on the graphics card");

    // ---- the driver's NGX core
    const unsigned long long appId = 0x24480451ull;
    // the same list as the engine's (nr::ngxpaths::SearchList: NGX keeps the first Init's paths, so every addon of ours passes the union)
    const std::vector<std::wstring> searchList = nr::ngxpaths::SearchList({ lsDir, exeDir });
    const std::vector<const wchar_t*> searchPaths = nr::ngxpaths::AsArray(searchList);
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = searchPaths.data();
    info.PathListInfo.Length = static_cast<unsigned>(searchPaths.size());
    info.LoggingInfo.LoggingCallback = NgxLog;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = false;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(appId, dataDir.c_str(), gpu.dev, &info, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) { char w[160]; snprintf(w, sizeof w, "the NVIDIA driver's NGX core did not start (%s)", Name(r)); return Finish(12, "NGX_CORE", w); }
    NVSDK_NGX_Parameter* caps = nullptr;
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
    if (NVSDK_NGX_FAILED(r) || !caps) { char w[160]; snprintf(w, sizeof w, "the NGX core gave no parameter block (%s)", Name(r)); return Finish(12, "NGX_CORE", w); }

    // ---- the helper DLL, then the model through it
    Helper helper;
    if (!helper.Load(exeDir + L"\\" + NR_FORWARDER_FILENAME)) return Finish(13, "HELPER", "the helper DLL is missing or is from another version of this addon");
    const int bits = helper.probe(model.c_str());
    Say("model probe: 0x%x (0xf is a usable model)", bits);
    if ((bits & 0xF) != 0xF) return Finish(14, "MODEL_LOAD", "the model file is missing or is not a usable model");

    // The model reads its floats through getter 14: use the setter slot that round-trips a float through it
    int floatSlot = -1;
    for (int slot : { 6, 5, 1, 2, 4, 7 }) {
        helper.probeFloat(caps, "NR.Probe", 1.5f, slot);
        alignas(8) uint8_t back[8] = {};
        const int got = helper.getFloat(caps, "NR.Probe", back, 14);
        float value = 0.0f;
        memcpy(&value, back, 4);
        helper.probeFloat(caps, "NR.Probe", 0.0f, slot);
        if (got == 1 && fabsf(value - 1.5f) < 1e-6f) { floatSlot = slot; break; }
    }
    if (floatSlot < 0) return Finish(15, "FLOAT_SLOT", "the model's parameter block did not behave as expected");
    helper.setFloatSlot(floatSlot);

    const int initResult = helper.init(model.c_str(), dataDir.c_str(), gpu.dev, caps);
    if (initResult != 1) { char w[160]; snprintf(w, sizeof w, "the model loaded but refused to start (%s)", Name(initResult)); return Finish(16, "MODEL_INIT", w); }

    // ---- a synthetic picture: colour gradient with a checker, flat depth, no motion
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
        uint8_t* p = &rgba[(static_cast<size_t>(y) * width + x) * 4];
        p[0] = static_cast<uint8_t>(x * 255 / width); p[1] = static_cast<uint8_t>(y * 255 / height); p[2] = ((x / 8 + y / 8) & 1) ? 217 : 38; p[3] = 255;
    }
    std::vector<float> depth(static_cast<size_t>(width) * height, 0.5f);
    std::vector<uint16_t> motion(static_cast<size_t>(width) * height * 2, 0);
    const auto shaderRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* color = Texture(gpu.dev, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* depthTex = Texture(gpu.dev, width, height, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* motionTex = Texture(gpu.dev, width, height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* output = Texture(gpu.dev, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!color || !depthTex || !motionTex || !output) return Finish(18, "FEATURE", "the graphics card ran out of memory for the test picture");
    Upload(gpu, color, width, height, 4, rgba.data(), shaderRead);
    Upload(gpu, depthTex, width, height, 4, depth.data(), shaderRead);
    Upload(gpu, motionTex, width, height, 4, motion.data(), shaderRead);
    gpu.Run();

    NrCreateParams create = {};
    create.width = width; create.height = height; create.preset = 0; create.scalingRatio = 1.0f;
    create.tuning = NrTuning{ 1u, 1u, 1u, 1.0f, 1.0f, 1.0f, 1.0f };
    void* feature = helper.create(gpu.list, caps, &create);
    const int createResult = helper.last(1);
    Say("create feature 18: %s", Name(createResult));
    gpu.Run();   // creating the feature records GPU work
    if (!feature) {
        if (createResult == static_cast<int>(NVSDK_NGX_Result_FAIL_FeatureNotSupported)) {
            // a refusal that depends on the size (or on how many times it was tried) looks different from one that does not: the same feature at other sizes, not run
            static const UINT sizes[][2] = { { 1920, 1080 }, { 960, 540 }, { 640, 360 }, { 2560, 1440 }, { 1280, 720 } };
            for (const auto& sz : sizes) {
                NrCreateParams c2 = create; c2.width = sz[0]; c2.height = sz[1];
                void* f2 = helper.create(gpu.list, caps, &c2);
                const int r2 = helper.last(1);
                gpu.Run();
                char line[96]; snprintf(line, sizeof line, "  %ux%u: %s\r\n", sz[0], sz[1], f2 ? "a feature was created" : Name(r2));
                g_report.variants += line;
                if (f2) helper.release(f2);
            }
            return Finish(17, "NOT_SUPPORTED", "this model file cannot create its feature on your graphics card");
        }
        char w[160]; snprintf(w, sizeof w, "the model failed to create its feature (%s)", Name(createResult));
        return Finish(18, "FEATURE", w);
    }

    NrEvalParams eval = {};
    eval.color = color; eval.depth = depthTex; eval.mvec = motionTex; eval.output = output;
    eval.width = width; eval.height = height; eval.guideWidth = width; eval.guideHeight = height; eval.depthInverted = 0;
    eval.mvScaleX = 1.0f; eval.mvScaleY = 1.0f; eval.scalingRatio = 1.0f; eval.tuning = create.tuning; eval.controlMask = nullptr;
    eval.reset = 1;
    int e = helper.evaluate(gpu.list, feature, caps, &eval);
    gpu.Run();
    if (e != 1) { char w[160]; snprintf(w, sizeof w, "the model failed when it ran (%s)", Name(e)); return Finish(19, "EVALUATE", w); }
    eval.reset = 0;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    const int runs = 3;
    for (int i = 0; i < runs; ++i) { helper.evaluate(gpu.list, feature, caps, &eval); gpu.Run(); }
    QueryPerformanceCounter(&t1);
    const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart) / runs;

    // ---- did it do anything to the picture?
    const std::vector<uint8_t> result = Readback(gpu, output, width, height, 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    uint64_t diff = 0;
    for (size_t i = 0; i < result.size(); i += 4) for (int k = 0; k < 3; ++k) diff += static_cast<uint64_t>(abs(static_cast<int>(result[i + k]) - static_cast<int>(rgba[i + k])));
    const double meanDiff = static_cast<double>(diff) / (static_cast<double>(result.size()) / 4.0 * 3.0);
    Say("the picture changed by %.2f of 255 on average; one run takes %.1f ms at %ux%u", meanDiff, ms, width, height);

    helper.release(feature);
    NVSDK_NGX_D3D12_Shutdown1(gpu.dev);
    if (meanDiff <= 0.5) return Finish(20, "UNCHANGED", "the model ran but did not change the test picture");
    char w[200];
    snprintf(w, sizeof w, "the model works on %s: it created its feature and changed the test picture (%.1f ms per run at %ux%u)", gpuName, ms, width, height);
    return Finish(0, "PASS", w);
}
