#include "updater.h"
#include "../addon/addon_security.h"
#include "../config/config_manager.h"
#include "../log/logger.h"
#include "../../sdk/include/eam/version.h"
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "version.lib")

namespace eam {
namespace update {

namespace {

constexpr const char* kSetupName = "LSAddonManagerSetup.exe";

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET x) : h(x) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete; Handle& operator=(const Handle&) = delete;
};

// A GET written straight to a file, with the bytes counted as they come; GitHub's download address answers with a redirect to its storage,
// which WinHTTP follows (never from HTTPS to HTTP). Gives up after `timeoutMs` without progress, or past `maxBytes`.
bool DownloadToFile(const std::wstring& url, const std::wstring& path, const std::atomic<bool>* cancel, std::atomic<uint64_t>* done,
                    std::atomic<uint64_t>* total, uint64_t maxBytes, std::string& error) {
    wchar_t host[256] = {}, urlPath[2048] = {}, extra[1024] = {};
    URL_COMPONENTS uc = {}; uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = urlPath; uc.dwUrlPathLength = 2048; uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS)) { error = "the address is not valid"; return false; }
    std::wstring agent = L"LSAddonManager/"; for (const char* c = EAM_VERSION_STRING; *c; ++c) agent += static_cast<wchar_t>(*c); agent += L" (update)";
    Handle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) { error = "the connection could not be opened"; return false; }
    WinHttpSetTimeouts(session.h, 30000, 30000, 30000, 30000);
    Handle connection(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!connection.h) { error = "the server could not be reached"; return false; }
    const std::wstring object = std::wstring(urlPath) + extra;
    Handle request(WinHttpOpenRequest(connection.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request.h || !WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.h, nullptr)) {
        error = "the download could not start (no connection to GitHub?)"; return false;
    }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) { error = "GitHub answered with HTTP " + std::to_string(status); return false; }
    wchar_t lengthText[32] = {}; DWORD lengthSize = sizeof lengthText;
    if (total && WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText, &lengthSize, WINHTTP_NO_HEADER_INDEX))
        *total = _wcstoui64(lengthText, nullptr, 10);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f) { error = "the file could not be written in the temporary folder"; return false; }
    std::vector<char> buffer(256 * 1024);
    uint64_t got = 0;
    bool ok = true;
    for (;;) {
        if (cancel && *cancel) { error = "cancelled"; ok = false; break; }
        DWORD read = 0;
        if (!WinHttpReadData(request.h, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) { error = "the download broke off"; ok = false; break; }
        if (read == 0) break;
        got += read;
        if (got > maxBytes) { error = "the download is larger than the release says"; ok = false; break; }
        if (fwrite(buffer.data(), 1, read, f) != read) { error = "the disk is full, or the file could not be written"; ok = false; break; }
        if (done) *done = got;
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

// Runs a program without a window and waits for it (up to `ms`); true when it ended with exit code 0.
bool RunHidden(const std::wstring& cmd, DWORD ms) {
    STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
    std::wstring line = cmd;
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    const bool ended = WaitForSingleObject(pi.hProcess, ms) == WAIT_OBJECT_0;
    DWORD code = 1;
    if (ended) GetExitCodeProcess(pi.hProcess, &code); else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return ended && code == 0;
}

// The Setup exe says what it is in its version resource: "LS Addon Manager Setup", and the version it installs.
std::string SetupIdentity(const std::wstring& exe, std::string& version) {
    DWORD handle = 0; const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &handle);
    if (!size) return {};
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data())) return {};
    auto text = [&](const wchar_t* key) {
        wchar_t query[128]; swprintf(query, 128, L"\\StringFileInfo\\040904b0\\%s", key);
        wchar_t* v = nullptr; UINT n = 0;
        return VerQueryValueW(data.data(), query, reinterpret_cast<void**>(&v), &n) && v && n ? Narrow(v) : std::string();
    };
    version = text(L"ProductVersion");
    return text(L"ProductName");
}

std::mutex g_mu;
Progress g_progress;                 // guarded by g_mu (done/total below are live)
std::atomic<bool> g_busy{ false }, g_cancel{ false };
std::atomic<uint64_t> g_done{ 0 }, g_total{ 0 };
std::wstring g_testUrl;              // guarded by g_mu

} // namespace

Progress DownloadAndCheck(const std::string& version, const std::wstring& url, const std::string& sha256, uint64_t size, const std::wstring& folder,
                          const std::atomic<bool>* cancel, std::atomic<uint64_t>* done, std::atomic<uint64_t>* total) {
    Progress p; p.version = version; p.step = Phase::Failed;
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::wstring zip = folder + L"\\LSAddonManager-" + Wide(version) + L"-x64.zip";
    const std::wstring setup = folder + L"\\" + Wide(kSetupName);
    DeleteFileW(setup.c_str());   // an older one from an earlier try: this one is unpacked from the zip checked now
    const uint64_t cap = size ? size : 1024ull * 1024 * 1024;
    if (!DownloadToFile(url, zip, cancel, done, total, cap, p.error)) return p;
    // the zip as GitHub listed it
    WIN32_FILE_ATTRIBUTE_DATA a{};
    const uint64_t got = GetFileAttributesExW(zip.c_str(), GetFileExInfoStandard, &a) ? (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow : 0;
    if (size && got != size) { p.error = "the download is not the size GitHub lists (" + std::to_string(got) + " of " + std::to_string(size) + " bytes)"; return p; }
    if (!sha256.empty()) {
        const std::string actual = AddonSecurity::ComputeSHA256(zip);
        if (_stricmp(actual.c_str(), sha256.c_str()) != 0) { p.error = "the download does not match GitHub's SHA-256 for it; it was not used"; DeleteFileW(zip.c_str()); return p; }
    }
    // Setup, out of the zip, and what it says it is
    wchar_t sys[MAX_PATH] = {}; GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + folder + L"\" " + Wide(kSetupName);
    if (!RunHidden(cmd, 120000) || GetFileAttributesW(setup.c_str()) == INVALID_FILE_ATTRIBUTES) { p.error = "Setup could not be unpacked from the zip"; return p; }
    std::string setupVersion;
    const std::string name = SetupIdentity(setup, setupVersion);
    if (name != "LS Addon Manager Setup" || setupVersion != version) {
        p.error = "the Setup in the zip is not LS Addon Manager " + version + "'s (it says \"" + name + "\" " + setupVersion + ")"; return p;
    }
    p.setup = setup; p.step = Phase::Ready;
    return p;
}

void SetDownloadUrlForTest(const wchar_t* url) { std::lock_guard<std::mutex> lk(g_mu); g_testUrl = url ? url : L""; }

void StartDownload(const Status& available) {
    if (available.state != State::Available || available.latest.empty()) return;
    std::wstring url;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_busy || (g_progress.step == Phase::Ready && g_progress.version == available.latest)) return;
        url = g_testUrl.empty() ? Wide(available.zipUrl) : g_testUrl;
        g_progress = Progress{}; g_progress.version = available.latest;
        if (url.empty()) { g_progress.step = Phase::Failed; g_progress.error = "this release has no zip to download: use its page instead"; return; }
        g_progress.step = Phase::Downloading;
    }
    g_busy = true; g_cancel = false; g_done = 0; g_total = available.zipSize;
    wchar_t tmp[MAX_PATH] = {}; GetTempPathW(MAX_PATH, tmp);
    const std::wstring folder = std::wstring(tmp) + L"LSAddonManager-update\\" + Wide(available.latest);
    LOG_INFO("Update", "downloading %s", available.latest.c_str());
    // A thread of its own, not kept: the process may end without any shutdown (see update_check.cpp).
    std::thread([available, url, folder] {
        const Progress p = DownloadAndCheck(available.latest, url, available.zipSha256, available.zipSize, folder, &g_cancel, &g_done, &g_total);
        if (p.step == Phase::Ready) LOG_INFO("Update", "%s downloaded and checked", available.latest.c_str());
        else LOG_WARN("Update", "%s: %s", available.latest.c_str(), p.error.c_str());
        { std::lock_guard<std::mutex> lk(g_mu); g_progress = p; }
        g_busy = false;
    }).detach();
}

void CancelDownload() { g_cancel = true; }

Progress CurrentProgress() {
    std::lock_guard<std::mutex> lk(g_mu);
    Progress p = g_progress;
    if (p.step == Phase::Downloading) { p.done = g_done; p.total = g_total; }
    return p;
}

void ForgetDownload() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_busy) g_progress = Progress{};
}

bool StartSetup(const std::wstring& lsDir, std::string& error) {
    Progress p;
    { std::lock_guard<std::mutex> lk(g_mu); p = g_progress; }
    if (p.step != Phase::Ready || p.setup.empty()) { error = "nothing is downloaded yet"; return false; }
    // administrator rights only when the folder needs them (Program Files, some Steam libraries)
    const std::wstring probe = lsDir + L"\\.lsam_update_write_test_" + std::to_wstring(GetCurrentProcessId());
    const HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    const bool writable = h != INVALID_HANDLE_VALUE;
    if (writable) CloseHandle(h);
    // a folder that ends in a backslash would escape the closing quote: written twice (as Setup itself does)
    const std::wstring folder = !lsDir.empty() && lsDir.back() == L'\\' ? lsDir + L"\\" : lsDir;
    const std::wstring params = L"--folder \"" + folder + L"\" --update-when-closed --restart";
    SHELLEXECUTEINFOW sei{}; sei.cbSize = sizeof sei; sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = writable ? L"open" : L"runas"; sei.lpFile = p.setup.c_str(); sei.lpParameters = params.c_str(); sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        error = GetLastError() == ERROR_CANCELLED ? "administrator rights were not given" : "Setup could not be started";
        return false;
    }
    LOG_INFO("Update", "Setup %s started; it waits for Lossless Scaling to close%s", p.version.c_str(), writable ? "" : " (as administrator)");
    std::lock_guard<std::mutex> lk(g_mu);
    g_progress.step = Phase::Handed;
    return true;
}

bool Skipped(const std::string& version) {
    return !version.empty() && ConfigManager::Instance().GlobalGetOr<std::string>("updates", "skipped", "") == version;
}

void Skip(const std::string& version) {
    auto& cfg = ConfigManager::Instance();
    cfg.GlobalSet("updates", "skipped", version);
    cfg.Save();
    LOG_INFO("Update", "the offer for %s will not be shown again", version.c_str());
}

} // namespace update
} // namespace eam
