#include "update_check.h"
#include "updater.h"
#include "../config/config_manager.h"
#include "../log/logger.h"
#include "../../sdk/include/eam/version.h"
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <cctype>
#include <ctime>
#include <exception>
#include <mutex>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace eam {
namespace update {

const wchar_t* const kLatestReleaseUrl = L"https://api.github.com/repos/Echo-Storm/ls-addon-manager/releases/latest";
const char* const kReleasesPage = "https://github.com/Echo-Storm/ls-addon-manager/releases";

// ---------------------------------------------------------------------------------------------------------------------------------- versions

Version ParseVersion(const std::string& text) {
    Version v;
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == 'v' || text[i] == 'V')) ++i;
    int parts[3] = { 0, 0, 0 };
    int count = 0;
    while (count < 3) {
        if (i >= text.size() || !isdigit(static_cast<unsigned char>(text[i]))) break;
        long n = 0;
        while (i < text.size() && isdigit(static_cast<unsigned char>(text[i]))) { n = n * 10 + (text[i] - '0'); if (n > 100000) return Version(); ++i; }
        parts[count++] = static_cast<int>(n);
        if (i < text.size() && text[i] == '.' && count < 3 && i + 1 < text.size() && isdigit(static_cast<unsigned char>(text[i + 1]))) ++i; else break;
    }
    if (count < 2) return Version();   // "7" or "" is not a version of this project
    v.major = parts[0]; v.minor = parts[1]; v.patch = parts[2]; v.ok = true;
    return v;
}

int Compare(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------- the answer

namespace {
bool TagIsPlain(const std::string& tag) {
    if (tag.empty() || tag.size() > 40) return false;
    for (const char c : tag) if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_')) return false;
    return true;
}
}

Release ParseLatestRelease(const std::string& json) {
    Release r;
    const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return r;
    const auto tag = j.find("tag_name");
    if (tag == j.end() || !tag->is_string()) return r;
    r.tag = tag->get<std::string>();
    const Version v = ParseVersion(r.tag);
    if (!v.ok || !TagIsPlain(r.tag)) return Release();
    r.version = std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    const auto draft = j.find("draft"), pre = j.find("prerelease");
    r.draft = draft != j.end() && draft->is_boolean() && draft->get<bool>();
    r.prerelease = pre != j.end() && pre->is_boolean() && pre->get<bool>();
    // The page to open is this project's release page for that tag: never an address taken from the answer, so a wrong or hostile answer
    // cannot make the manager open anything else.
    r.url = std::string(kReleasesPage) + "/tag/" + r.tag;
    // the zip: found by its exact name among the assets; only its size and SHA-256 are taken from the answer
    r.zipName = "LSAddonManager-" + r.version + "-x64.zip";
    if (const auto assets = j.find("assets"); assets != j.end() && assets->is_array())
        for (const auto& asset : *assets) {
            if (!asset.is_object()) continue;
            const auto name = asset.find("name");
            if (name == asset.end() || !name->is_string() || name->get<std::string>() != r.zipName) continue;
            if (const auto size = asset.find("size"); size != asset.end() && size->is_number_unsigned()) r.zipSize = size->get<uint64_t>();
            if (const auto digest = asset.find("digest"); digest != asset.end() && digest->is_string()) {
                const std::string d = digest->get<std::string>();
                if (d.rfind("sha256:", 0) == 0 && d.size() == 7 + 64 && d.find_first_not_of("0123456789abcdef", 7) == std::string::npos) r.zipSha256 = d.substr(7);
            }
            r.zipUrl = std::string(kReleasesPage) + "/download/" + r.tag + "/" + r.zipName;
        }
    r.ok = true;
    return r;
}

bool ShouldCheckNow(bool enabled, int64_t lastCheck, int64_t now, int64_t intervalSeconds) {
    if (!enabled) return false;
    if (lastCheck <= 0 || lastCheck > now) return true;
    return now - lastCheck >= intervalSeconds;
}

// ---------------------------------------------------------------------------------------------------------------------------------- fetching

namespace {

std::string WinHttpErrorText(DWORD e) {
    switch (e) {
    case ERROR_WINHTTP_TIMEOUT: return "the server did not answer in time";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "the server could not be found (are you offline?)";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "could not connect to the server";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "the connection was lost";
    case ERROR_WINHTTP_SECURE_FAILURE: return "the secure connection could not be made";
    case ERROR_WINHTTP_INVALID_URL: return "the address is not valid";
    default: return "network error " + std::to_string(e);
    }
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle() = default;
    explicit Handle(HINTERNET x) : h(x) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::wstring UserAgent() {
    std::wstring ua = L"LSAddonManager/";
    for (const char* c = EAM_VERSION_STRING; *c; ++c) ua += static_cast<wchar_t>(*c);
    return ua + L" (update check)";
}

} // namespace

FetchResult HttpGet(const std::wstring& url, unsigned timeoutMs, size_t maxBytes) {
    FetchResult r;
    wchar_t host[256] = {}, path[2048] = {}, extra[1024] = {};
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = static_cast<DWORD>(sizeof host / sizeof host[0]);
    uc.lpszUrlPath = path; uc.dwUrlPathLength = static_cast<DWORD>(sizeof path / sizeof path[0]);
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = static_cast<DWORD>(sizeof extra / sizeof extra[0]);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS)) {
        r.error = "the address is not valid";
        return r;
    }
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs) * 3;

    Handle session(WinHttpOpen(UserAgent().c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) { r.error = WinHttpErrorText(GetLastError()); return r; }
    WinHttpSetTimeouts(session.h, static_cast<int>(timeoutMs), static_cast<int>(timeoutMs), static_cast<int>(timeoutMs), static_cast<int>(timeoutMs));
    Handle connection(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!connection.h) { r.error = WinHttpErrorText(GetLastError()); return r; }
    const std::wstring object = std::wstring(path) + extra;
    Handle request(WinHttpOpenRequest(connection.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request.h) { r.error = WinHttpErrorText(GetLastError()); return r; }
    const wchar_t* headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request.h, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.h, nullptr)) {
        r.error = WinHttpErrorText(GetLastError());
        return r;
    }
    DWORD status = 0, size = sizeof status;
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        r.error = WinHttpErrorText(GetLastError());
        return r;
    }
    r.status = static_cast<int>(status);
    for (;;) {
        if (GetTickCount64() > deadline) { r.error = "the server took too long"; return r; }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) { r.error = WinHttpErrorText(GetLastError()); return r; }
        if (available == 0) break;
        if (r.body.size() + available > maxBytes) { r.error = "the answer was larger than expected"; return r; }
        const size_t at = r.body.size();
        r.body.resize(at + available);
        DWORD got = 0;
        if (!WinHttpReadData(request.h, &r.body[at], available, &got)) { r.error = WinHttpErrorText(GetLastError()); return r; }
        r.body.resize(at + got);
    }
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------------------------------------------------------------- the check

Status Check(const std::string& currentVersion, const std::wstring& url, unsigned timeoutMs) {
    Status s;
    s.current = currentVersion;
    s.checkedAt = static_cast<int64_t>(time(nullptr));
    const FetchResult f = HttpGet(url, timeoutMs, 256 * 1024);
    if (!f.ok) { s.state = State::Failed; s.error = f.error; return s; }
    if (f.status != 200) {
        s.state = State::Failed;
        s.error = f.status == 403 || f.status == 429 ? "GitHub is limiting requests for now; try again later" : "GitHub answered with HTTP " + std::to_string(f.status);
        return s;
    }
    const Release rel = ParseLatestRelease(f.body);
    if (!rel.ok) { s.state = State::Failed; s.error = "the answer could not be read"; return s; }
    s.latest = rel.version;
    const Version now = ParseVersion(currentVersion), latest = ParseVersion(rel.version);
    if (!rel.draft && !rel.prerelease && now.ok && Compare(latest, now) > 0) {
        s.state = State::Available; s.url = rel.url; s.zipUrl = rel.zipUrl; s.zipSha256 = rel.zipSha256; s.zipSize = rel.zipSize;
    }
    else s.state = State::UpToDate;
    return s;
}

namespace {
std::mutex g_mu;
Status g_status;                       // guarded by g_mu
std::wstring g_url = kLatestReleaseUrl;
std::atomic<bool> g_busy{ false };     // a flag, not a std::thread object kept here: the process can end without any shutdown (see the exit-crash notes)
std::string g_announced;               // the version the notice was last made for in this run
}

void SetStatusForTest(const Status& st) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_status = st;
}

void SetUrlForTest(const wchar_t* url) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_url = url ? url : kLatestReleaseUrl;
}

Status Current() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_status;
}

void StartCheckAsync() {
    if (g_busy.exchange(true)) return;
    std::wstring url;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_status.state = State::Checking;
        g_status.error.clear();
        url = g_url;
    }
    std::thread([url] {
        try {
            const Status s = Check(EAM_VERSION_STRING, url);
            if (s.state == State::Failed) LOG_INFO("Update", "The update check failed: %s", s.error.c_str());
            else LOG_INFO("Update", "The update check found %s (this is %s)", s.latest.c_str(), s.current.c_str());
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_status = s;
            }
            auto& cfg = ConfigManager::Instance();   // remembered even when it failed, so an offline machine is not asked again every minute
            cfg.GlobalSet("updates", "last_check", static_cast<int64_t>(s.checkedAt));
            cfg.Save();
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_status.state = State::Failed;
            g_status.error = std::string("unexpected problem: ") + e.what();
        } catch (...) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_status.state = State::Failed;
            g_status.error = "unexpected problem";
        }
        g_busy = false;
    }).detach();
}

void Tick() {
    static ULONGLONG cleanedAt = 0;   // the updater's old downloads, once a day (updater.h)
    if (!cleanedAt || GetTickCount64() - cleanedAt > 24ull * 3600 * 1000) { cleanedAt = GetTickCount64(); CleanOldDownloads(""); }
    auto& cfg = ConfigManager::Instance();
    const bool enabled = cfg.GlobalGetOr<bool>("updates", "check", true);   // on unless the person turned it off
    const int64_t last = cfg.GlobalGetOr<int64_t>("updates", "last_check", 0);
    if (ShouldCheckNow(enabled, last, static_cast<int64_t>(time(nullptr)))) StartCheckAsync();
}

std::string TakeNotice() {
    const Status s = Current();
    if (s.state != State::Available || s.latest.empty()) return std::string();
    auto& cfg = ConfigManager::Instance();
    const std::string seen = cfg.GlobalGetOr<std::string>("updates", "announced", "");
    if (seen == s.latest || g_announced == s.latest) return std::string();
    g_announced = s.latest;
    cfg.GlobalSet("updates", "announced", s.latest);
    cfg.Save();
    return "Version " + s.latest + " is available (you have " + s.current + "). See the About tab.";
}

std::string DescribeStatus(const Status& s) {
    switch (s.state) {
    case State::Idle: return "Not checked yet.";
    case State::Checking: return "Checking...";
    case State::UpToDate: return "You have the latest version (" + s.current + ").";
    case State::Available: return "Version " + s.latest + " is available. You have " + s.current + ".";
    case State::Failed: return "Could not check: " + s.error + ".";
    }
    return std::string();
}

} // namespace update
} // namespace eam
