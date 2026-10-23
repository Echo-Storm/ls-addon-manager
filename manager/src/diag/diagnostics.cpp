#include "diagnostics.h"
#include <windows.h>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace eam {

static constexpr uintmax_t kMaxLog = 3u * 1024 * 1024;

// Copy a file, keeping only its newest kMaxLog bytes when it is bigger.
static bool CopyCapped(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(from, ec);
    if (ec) return false;
    if (size <= kMaxLog) { fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec); return !ec; }
    std::ifstream in(from, std::ios::binary);
    if (!in) return false;
    in.seekg((std::streamoff)(size - kMaxLog));
    std::vector<char> buf(kMaxLog);
    in.read(buf.data(), (std::streamsize)kMaxLog);
    std::ofstream out(to, std::ios::binary);
    const char note[] = "[the start of this log was cut: only the newest 3 MB are included]\n";
    out.write(note, sizeof note - 1);
    out.write(buf.data(), in.gcount());
    return (bool)out;
}

static bool RunHidden(std::wstring cmd, DWORD timeoutMs) {
    STARTUPINFOW si{ sizeof si };
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code == 0;
}

DiagResult CreateDiagnosticsZip(const fs::path& lsDir, const std::string& summary, const fs::path& outDir, const std::function<void(const fs::path&)>& extras, const std::wstring& prefix) {
    DiagResult r;
    std::error_code ec;
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t stamp[32]; swprintf(stamp, 32, L"%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    const fs::path stage = fs::path(tmp) / (L"eam-diag-" + std::wstring(stamp) + L"-" + std::to_wstring(GetTickCount64()));
    const fs::path bundle = stage / L"LSAddonManager-diagnostics";
    fs::create_directories(bundle, ec);
    if (ec) { r.message = "Could not create a temporary folder: " + ec.message(); return r; }

    { std::ofstream info(bundle / "info.txt", std::ios::binary); info << summary; }
    int copied = 0;
    const fs::path logs = lsDir / "logs";
    if (fs::is_directory(logs, ec)) {
        fs::create_directories(bundle / "logs", ec);
        for (const auto& e : fs::directory_iterator(logs, ec)) if (e.is_regular_file() && (e.path().extension() == ".log" || e.path().extension() == ".old")) { if (CopyCapped(e.path(), bundle / "logs" / e.path().filename())) ++copied; }
    }
    const fs::path addons = lsDir / "addons";
    if (fs::is_directory(addons, ec)) {
        if (fs::exists(addons / "config.json", ec)) { fs::copy_file(addons / "config.json", bundle / "config.json", fs::copy_options::overwrite_existing, ec); if (!ec) ++copied; }
        for (const auto& d : fs::directory_iterator(addons, ec)) {   // an addon's own logs (for example the NGX log next to the addon)
            if (!d.is_directory() || d.path().filename().wstring().rfind(L".", 0) == 0) continue;
            for (const auto& f : fs::directory_iterator(d.path(), ec)) if (f.is_regular_file() && f.path().extension() == ".log") {
                fs::create_directories(bundle / "addon-logs" / d.path().filename(), ec);
                if (CopyCapped(f.path(), bundle / "addon-logs" / d.path().filename() / f.path().filename())) ++copied;
            }
        }
    }

    if (extras) extras(bundle);
    fs::create_directories(outDir, ec);
    r.zip = outDir / (prefix + L"-" + std::wstring(stamp) + L".zip");
    wchar_t sysDir[MAX_PATH] = {};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    const std::wstring cmd = L"\"" + std::wstring(sysDir) + L"\\tar.exe\" -a -cf \"" + r.zip.wstring() + L"\" -C \"" + stage.wstring() + L"\" LSAddonManager-diagnostics";
    const bool ok = RunHidden(cmd, 60000) && fs::exists(r.zip, ec);
    fs::remove_all(stage, ec);   // our own temporary staging folder
    if (!ok) { r.message = "Could not create the zip (Windows' tar.exe failed)."; r.zip.clear(); return r; }
    r.ok = true;
    r.message = "Saved " + r.zip.filename().string() + " (" + std::to_string(copied) + " files plus a summary). Nothing was uploaded.";
    return r;
}

} // namespace eam
