#include "report.h"
#include "history.h"
#include "../event/event_system.h"
#include "../host/metrics.h"
#include "../log/logger.h"
#include "../../sdk/include/eam/events.h"
#include "../../sdk/include/eam/version.h"
#include <windows.h>
#include <dxgi.h>
#include <fstream>
#include <regex>
#include <sstream>
#pragma comment(lib, "version.lib")
#pragma comment(lib, "dxgi.lib")

namespace fs = std::filesystem;

namespace eam {

namespace {

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string WindowsVersion() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{ sizeof v };
    if (const auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")); fn && fn(&v) == 0) {
        char b[64]; snprintf(b, sizeof b, "Windows %lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber); return b;
    }
    return "Windows (version unknown)";
}

std::string FileVersion(const fs::path& file) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (!size) return "?";
    std::vector<char> data(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, data.data())) return "?";
    VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi) return "?";
    char b[64]; snprintf(b, sizeof b, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS)); return b;
}

std::string FileTime(const fs::path& file) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &a)) return "?";
    FILETIME local; SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&a.ftLastWriteTime, &local) || !FileTimeToSystemTime(&local, &st)) return "?";
    char b[32]; snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute); return b;
}

std::string SystemText(const fs::path& lsDir) {
    std::ostringstream o;
    o << WindowsVersion() << "\n";
    o << "Lossless Scaling: " << FileVersion(lsDir / "LosslessScaling.exe") << "  (folder " << Narrow(lsDir.wstring()) << ")\n";
    o << "Manager: " << EAM_VERSION_STRING << " (API " << EAM_API_VERSION_STRING << "), Lossless.dll " << FileTime(lsDir / "Lossless.dll") << "\n\n";
    o << "Graphics cards (DXGI):\n";
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
            LARGE_INTEGER umd{}; const bool haveDriver = SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
            char drv[64] = "?"; if (haveDriver) snprintf(drv, sizeof drv, "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart));
            o << "  " << i << ": " << Narrow(d.Description) << "  vendor 0x" << std::hex << d.VendorId << std::dec << "  video memory " << (d.DedicatedVideoMemory >> 20) << " MB  driver " << drv
              << "  LUID " << std::hex << d.AdapterLuid.HighPart << ":" << d.AdapterLuid.LowPart << std::dec << ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? "  (software)" : "") << "\n";
            a->Release();
        }
        factory->Release();
    }
    o << "\nDisplays:\n";
    DISPLAY_DEVICEW dd{ sizeof dd };
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        DEVMODEW dm{}; dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
            o << "  " << Narrow(dd.DeviceName) << ": " << dm.dmPelsWidth << "x" << dm.dmPelsHeight << " at " << dm.dmDisplayFrequency << " Hz" << ((dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " (primary)" : "") << "\n";
    }
    o << "\nAddons (addons folder):\n";
    std::error_code ec;
    for (const auto& d : fs::directory_iterator(lsDir / "addons", ec)) {
        if (!d.is_directory() || d.path().filename().wstring().rfind(L".", 0) == 0) continue;
        std::string version = "?";
        { std::ifstream in(d.path() / "addon.json"); std::stringstream ss; ss << in.rdbuf(); std::smatch m; const std::string t = ss.str();
          if (std::regex_search(t, m, std::regex("\"version\"\\s*:\\s*\"([^\"]+)\""))) version = m[1]; }
        std::string dll = "(no dll)", when;
        for (const auto& f : fs::directory_iterator(d.path(), ec)) if (f.is_regular_file() && f.path().extension() == ".dll") {
            const std::string n = Narrow(f.path().filename().wstring());
            if (n.rfind("nvngx", 0) == 0 || n.rfind("amd_", 0) == 0 || n.rfind("libxess", 0) == 0) continue;
            dll = n; when = FileTime(f.path()); break;
        }
        o << "  " << Narrow(d.path().filename().wstring()) << "  version " << version << "  " << dll << "  " << when << "\n";
    }
    return o.str();
}

std::string MetricsCsv() {
    std::ostringstream o;
    o << "addon,key,unit,seconds_ago,value\n";
    const Metrics& m = Metrics::Instance();
    const double now = m.Now();
    for (const Metrics::Series& s : m.Snapshot(600.0))
        for (const Metrics::Sample& p : s.samples) o << s.addon << "," << s.key << "," << s.unit << "," << (now - p.t) << "," << p.v << "\n";
    return o.str();
}

} // namespace

DiagResult MakeReport(const fs::path& lsDir, const std::string& reason, const fs::path& outDir) {
    history::Add("report", reason);
    std::ostringstream summary;
    summary << "LS Addon Manager report\nReason: " << reason << "\nManager " << EAM_VERSION_STRING << " (API " << EAM_API_VERSION_STRING << ")\n\n"
            << "Files: system.txt (Windows, cards, displays, versions), history.txt (what Lossless Scaling's window sent, scaling starts and stops, errors), metrics.csv (the last ten minutes of each series),\n"
            << "       logs\\ and addon-logs\\ (every log), config.json (the manager's settings), *-frametrace.csv (each addon's frame trace: every present, upscaler pass and model run).\n"
            << "Nothing was uploaded: send the file yourself.\n";
    const auto extras = [&](const fs::path& bundle) {
        { std::ofstream f(bundle / "system.txt", std::ios::binary); f << SystemText(lsDir); }
        { std::ofstream f(bundle / "history.txt", std::ios::binary); f << history::Dump(); }
        { std::ofstream f(bundle / "metrics.csv", std::ios::binary); f << MetricsCsv(); }
        { std::error_code ec; const fs::path ini = lsDir / "config.ini"; if (fs::exists(ini, ec)) fs::copy_file(ini, bundle / "config.ini", fs::copy_options::overwrite_existing, ec); }
        const std::wstring folder = bundle.wstring(); const std::string why = reason;
        EamReportData data{ folder.c_str(), why.c_str() };
        EventBus::Instance().Publish(EAM_EVENT_REPORT, &data, sizeof data);   // the addons write their own files into the folder (called on this thread, before the zip is made)
    };
    return CreateDiagnosticsZip(lsDir, summary.str(), outDir, extras, L"LSAddonManager-report");
}

} // namespace eam
