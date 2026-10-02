#include "addon_install.h"
#include <windows.h>
#include <cwctype>
#include <vector>

namespace fs = std::filesystem;

namespace eam {

static std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    std::string s((size_t)WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], (int)s.size(), nullptr, nullptr);
    return s;
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

// The folder that holds the addon: `root` itself, or its single child folder, when it has an addon.json or a .dll.
static fs::path FindAddonRoot(const fs::path& root) {
    auto hasAddon = [](const fs::path& d) {
        std::error_code ec;
        if (fs::exists(d / "addon.json", ec)) return true;
        for (const auto& e : fs::directory_iterator(d, ec)) if (e.path().extension() == ".dll") return true;
        return false;
    };
    if (hasAddon(root)) return root;
    std::error_code ec; fs::path only; int n = 0;
    for (const auto& e : fs::directory_iterator(root, ec)) if (e.is_directory()) { only = e.path(); ++n; }
    if (n == 1 && hasAddon(only)) return only;
    return {};
}

static std::wstring SafeFolderName(std::wstring name) {
    for (auto& c : name) if (!(std::iswalnum(c) || c == L'-' || c == L'_' || c == L'.' || c == L' ')) c = L'_';
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    while (!name.empty() && name.front() == L'.') name.erase(name.begin());
    return name;
}

PlaceResult PlaceAddon(const fs::path& src, const fs::path& addonsDir) {
    PlaceResult r;
    std::error_code ec;
    if (!fs::exists(src, ec)) { r.message = "That file or folder does not exist."; return r; }

    fs::path staging, srcDir;
    std::wstring name;
    const std::wstring ext = src.extension().wstring();
    const bool isZip = _wcsicmp(ext.c_str(), L".zip") == 0, isDll = _wcsicmp(ext.c_str(), L".dll") == 0;
    auto cleanup = [&] { if (!staging.empty()) { std::error_code e; fs::remove_all(staging, e); staging.clear(); } };   // our own temporary extraction

    if (fs::is_directory(src, ec)) {
        srcDir = FindAddonRoot(src);
        if (srcDir.empty()) { r.message = "No addon found in that folder: it needs an addon.json or a .dll."; return r; }
        name = srcDir.filename().wstring();
    } else if (isZip || isDll) {
        staging = addonsDir / (L".install-" + std::to_wstring(GetTickCount64()));
        fs::create_directories(staging, ec);
        if (isDll) {
            const fs::path d = staging / src.stem();
            fs::create_directories(d, ec);
            fs::copy_file(src, d / src.filename(), fs::copy_options::overwrite_existing, ec);
            if (ec) { r.message = "Could not copy the DLL: " + ec.message(); cleanup(); return r; }
            srcDir = d; name = src.stem().wstring();
        } else {
            wchar_t sysDir[MAX_PATH] = {};
            GetSystemDirectoryW(sysDir, MAX_PATH);
            const std::wstring cmd = L"\"" + std::wstring(sysDir) + L"\\tar.exe\" -xf \"" + src.wstring() + L"\" -C \"" + staging.wstring() + L"\"";
            if (!RunHidden(cmd, 60000)) { r.message = "Could not unpack the zip (Windows' tar.exe failed)."; cleanup(); return r; }
            srcDir = FindAddonRoot(staging);
            if (srcDir.empty()) { r.message = "No addon found in that zip: it needs an addon.json or a .dll."; cleanup(); return r; }
            name = (srcDir == staging) ? src.stem().wstring() : srcDir.filename().wstring();
        }
    } else {
        r.message = "Pick an addon folder, a .zip or a .dll."; return r;
    }

    name = SafeFolderName(name);
    if (name.empty()) { r.message = "Could not work out a folder name for the addon."; cleanup(); return r; }
    const fs::path dest = addonsDir / name;
    if (fs::exists(dest, ec)) {
        r.message = "An addon folder named '" + Utf8(name) + "' is already installed. Remove or rename it first (an addon that is loaded cannot be replaced while Lossless Scaling runs).";
        cleanup(); return r;
    }
    fs::create_directories(dest, ec);
    fs::copy(srcDir, dest, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
    cleanup();
    if (ec) {
        const std::string why = ec.message();
        fs::remove_all(dest, ec);   // the folder made just above, half filled: a second try would find it and say the addon is already installed
        r.message = "Could not copy the addon: " + why;
        return r;
    }
    r.ok = true; r.dest = dest;
    return r;
}

RemoveResult MoveAddonToRemoved(const fs::path& folder, const fs::path& addonsDir) {
    RemoveResult r;
    std::error_code ec;
    const fs::path f = fs::weakly_canonical(folder, ec), a = fs::weakly_canonical(addonsDir, ec);
    if (!fs::is_directory(folder, ec)) { r.message = "That addon folder no longer exists."; return r; }
    if (f.parent_path() != a || f.filename().empty() || f.filename().wstring().rfind(L".", 0) == 0) {
        r.message = "That folder is not an addon folder inside the addons folder."; return r;
    }
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t stamp[32]; swprintf(stamp, 32, L"%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const fs::path bin = addonsDir / L".removed";
    fs::create_directories(bin, ec);
    fs::path dest = bin / (f.filename().wstring() + L"-" + stamp);
    for (int i = 2; fs::exists(dest, ec); ++i) dest = bin / (f.filename().wstring() + L"-" + stamp + L"-" + std::to_wstring(i));
    fs::rename(folder, dest, ec);
    if (ec) {
        r.message = "Could not move the addon folder (it may still be in use; restart Lossless Scaling and try again): " + ec.message();
        return r;
    }
    r.ok = true; r.movedTo = dest;
    return r;
}

} // namespace eam
