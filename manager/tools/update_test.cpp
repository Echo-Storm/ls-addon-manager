// Offline test of the update check: version numbers, reading GitHub's answer, when a check is due, and the whole check against a small server of
// its own on the loopback address (a good answer, a newer one, errors, silence, a huge answer), plus the worker that runs it and what it remembers.
//   eam_updatetest.exe          everything above; needs no internet
//   eam_updatetest.exe live     also asks the real GitHub once and prints what it says
#include "src/config/config_manager.h"
#include "src/log/logger.h"
#include "src/update/update_check.h"
#include "eam/version.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "src/update/updater.h"
#include "src/addon/addon_security.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace fs = std::filesystem;
using namespace eam;
using namespace eam::update;

static int g_failed = 0;
static void Check(const char* what, bool ok, const std::string& detail = "") {
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  (", detail.empty() ? "" : (detail + ")").c_str());
    if (!ok) ++g_failed;
}
static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// ---- a server on 127.0.0.1 that plays a script, one connection per step
struct Step {
    enum Kind { Reply, Close, Hang } kind = Reply;
    int status = 200;
    std::string body;
};

class Server {
public:
    bool Start(std::vector<Step> script) {
        m_script = std::move(script);
        m_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        a.sin_port = 0;
        if (bind(m_listen, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(m_listen, 4) != 0) return false;
        int len = sizeof a;
        getsockname(m_listen, reinterpret_cast<sockaddr*>(&a), &len);
        m_port = ntohs(a.sin_port);
        m_thread = std::thread([this] { Run(); });
        return true;
    }
    ~Server() { Stop(); }
    void Stop() {
        m_stop = true;
        if (m_listen != INVALID_SOCKET) { closesocket(m_listen); m_listen = INVALID_SOCKET; }
        if (m_thread.joinable()) m_thread.join();
    }
    std::wstring Url(const char* path = "/repos/Echo-Storm/ls-addon-manager/releases/latest") const {
        return L"http://127.0.0.1:" + std::to_wstring(m_port) + std::wstring(path, path + strlen(path));
    }
    std::string LastRequest() { std::lock_guard<std::mutex> lk(m_mu); return m_last; }
    int Served() const { return m_served; }

private:
    void Run() {
        for (const Step& step : m_script) {
            fd_set fds; FD_ZERO(&fds); FD_SET(m_listen, &fds);
            timeval tv = { 30, 0 };
            if (m_stop || select(0, &fds, nullptr, nullptr, &tv) <= 0) return;
            SOCKET c = accept(m_listen, nullptr, nullptr);
            if (c == INVALID_SOCKET) return;
            std::string req;
            char buf[2048];
            while (req.find("\r\n\r\n") == std::string::npos) {
                const int n = recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                req.append(buf, buf + n);
            }
            { std::lock_guard<std::mutex> lk(m_mu); m_last = req; }
            if (step.kind == Step::Reply) {
                char head[256];
                snprintf(head, sizeof head, "HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", step.status, step.body.size());
                send(c, head, static_cast<int>(strlen(head)), 0);
                size_t at = 0;
                while (at < step.body.size()) { const int n = send(c, step.body.data() + at, static_cast<int>((std::min)(step.body.size() - at, static_cast<size_t>(64 * 1024))), 0); if (n <= 0) break; at += n; }
            } else if (step.kind == Step::Hang) {
                for (int i = 0; i < 60 && !m_stop; ++i) Sleep(100);   // says nothing for up to six seconds
            }
            closesocket(c);
            ++m_served;
        }
    }
    std::vector<Step> m_script;
    SOCKET m_listen = INVALID_SOCKET;
    int m_port = 0;
    std::thread m_thread;
    std::mutex m_mu;
    std::string m_last;
    std::atomic<bool> m_stop{ false };
    std::atomic<int> m_served{ 0 };
};

static std::string Answer(const char* tag, bool prerelease = false, bool draft = false) {
    // the shape of GitHub's answer, with the extra fields it really sends (and a hostile address that must be ignored)
    return std::string("{\"url\":\"https://api.github.com/repos/Echo-Storm/ls-addon-manager/releases/1\",\"html_url\":\"https://evil.example/download\",\"id\":1,\"tag_name\":\"") + tag +
           "\",\"name\":\"LS Addon Manager\",\"draft\":" + (draft ? "true" : "false") + ",\"prerelease\":" + (prerelease ? "true" : "false") +
           ",\"published_at\":\"2026-09-21T20:35:49Z\",\"assets\":[{\"name\":\"LSAddonManager-x64.zip\",\"size\":1424054}],\"body\":\"notes\"}";
}

static bool WaitNotChecking(int ms) {
    for (int t = 0; t < ms; t += 25) { if (Current().state != State::Checking) return true; Sleep(25); }
    return false;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const bool live = argc > 1 && !strcmp(argv[1], "live");
    const fs::path dir = fs::temp_directory_path() / ("eam_updatetest_" + std::to_string(GetCurrentProcessId()));
    { std::error_code fresh; fs::remove_all(dir, fresh); }   // a fresh folder every run (process ids are reused)
    fs::create_directories(dir);
    Logger::Instance().Init((dir / "test.log").wstring());
    ConfigManager::Instance().Load((dir / "config.json").wstring());
    const std::string me = EAM_VERSION_STRING;

    printf("== version numbers\n");
    {
        const Version a = ParseVersion("v0.4.1"), b = ParseVersion("0.4.1"), c = ParseVersion("0.4"), d = ParseVersion("0.5.0-beta.1"), e = ParseVersion(" V1.10.2 ");
        Check("v0.4.1 and 0.4.1 read the same", a.ok && b.ok && a.major == 0 && a.minor == 4 && a.patch == 1 && Compare(a, b) == 0);
        Check("0.4 reads as 0.4.0", c.ok && c.patch == 0);
        Check("a suffix such as -beta.1 is ignored", d.ok && d.major == 0 && d.minor == 5 && d.patch == 0);
        Check("spaces and an upper-case V are tolerated", e.ok && e.major == 1 && e.minor == 10 && e.patch == 2);
        Check("things that are not versions are refused", !ParseVersion("").ok && !ParseVersion("7").ok && !ParseVersion("abc").ok && !ParseVersion("v.1.2").ok && !ParseVersion("1.").ok && !ParseVersion("99999999999.1.1").ok);
        Check("numbers are compared as numbers: 0.10.0 is newer than 0.9.0", Compare(ParseVersion("0.10.0"), ParseVersion("0.9.0")) > 0 && Compare(ParseVersion("0.9.0"), ParseVersion("0.10.0")) < 0);
        Check("major, minor and patch each decide in turn", Compare(ParseVersion("1.0.0"), ParseVersion("0.99.99")) > 0 && Compare(ParseVersion("0.4.2"), ParseVersion("0.4.1")) > 0 && Compare(ParseVersion("0.4.1"), ParseVersion("0.4.1")) == 0);
        Check("this build's own version reads", ParseVersion(me).ok);
    }

    printf("== reading GitHub's answer\n");
    {
        const std::string good = Answer("v0.4.1");
        const auto r = ParseLatestRelease(good);
        Check("a real-shaped answer is read", r.ok && r.tag == "v0.4.1" && r.version == "0.4.1" && !r.draft && !r.prerelease);
        Check("the page to open is this project's, built here", r.url == std::string(kReleasesPage) + "/tag/v0.4.1", r.url);
        Check("an address inside the answer is never used", !Has(r.url, "evil"));
        Check("a draft and a prerelease are recognised", ParseLatestRelease(Answer("v9.0.0", false, true)).draft && ParseLatestRelease(Answer("v9.0.0", true)).prerelease);
        Check("a tag that is not a plain version is refused", !ParseLatestRelease("{\"tag_name\":\"v1.0.0/../../x\"}").ok && !ParseLatestRelease("{\"tag_name\":\"latest\"}").ok && !ParseLatestRelease("{\"tag_name\":\"\"}").ok);
        Check("no tag, not JSON, or the wrong kind of JSON is refused", !ParseLatestRelease("{}").ok && !ParseLatestRelease("<html>").ok && !ParseLatestRelease("").ok && !ParseLatestRelease("[1,2]").ok && !ParseLatestRelease("{\"tag_name\":5}").ok);
    }

    printf("== when a check is due\n");
    {
        const int64_t now = 2000000000, day = 24 * 3600;
        Check("switched off: never", !ShouldCheckNow(false, 0, now) && !ShouldCheckNow(false, now - 10 * day, now));
        Check("switched on and never checked: now", ShouldCheckNow(true, 0, now));
        Check("checked an hour ago: not yet", !ShouldCheckNow(true, now - 3600, now));
        Check("checked just under a day ago: not yet; a day ago: now", !ShouldCheckNow(true, now - day + 60, now) && ShouldCheckNow(true, now - day, now));
        Check("a last check in the future (clock set back) does not block for ever", ShouldCheckNow(true, now + 5 * day, now));
    }

    printf("== the whole check, against a server of its own\n");
    {
        Server s;
        Check("the test server starts", s.Start({ { Step::Reply, 200, Answer("v99.0.0") }, { Step::Reply, 200, Answer(("v" + me).c_str()) }, { Step::Reply, 200, Answer("v0.0.1") },
                                                  { Step::Reply, 200, Answer("v99.0.0", true) }, { Step::Reply, 404, "{\"message\":\"Not Found\"}" }, { Step::Reply, 403, "{}" },
                                                  { Step::Reply, 200, "this is not json" }, { Step::Close }, { Step::Hang }, { Step::Reply, 200, std::string(2 * 1024 * 1024, 'x') } }));
        Status st = Check(me, s.Url());
        Check("a newer release: Available, with this project's page", st.state == State::Available && st.latest == "99.0.0" && st.url == std::string(kReleasesPage) + "/tag/v99.0.0", DescribeStatus(st));
        const std::string req = s.LastRequest();
        Check("the request names the program and its version", Has(req, std::string("User-Agent: LSAddonManager/" + me).c_str()), req.substr(0, 120));
        Check("...asks for GitHub's JSON and sends nothing else about the person", Has(req, "Accept: application/vnd.github+json") && !Has(req, "Cookie") && !Has(req, "Authorization"));
        st = Check(me, s.Url());
        Check("the same release: UpToDate", st.state == State::UpToDate && st.latest == me, DescribeStatus(st));
        st = Check(me, s.Url());
        Check("an older release: UpToDate", st.state == State::UpToDate);
        st = Check(me, s.Url());
        Check("a newer prerelease is ignored", st.state == State::UpToDate);
        st = Check(me, s.Url());
        Check("HTTP 404: Failed, saying so", st.state == State::Failed && Has(st.error, "404"), st.error);
        st = Check(me, s.Url());
        Check("HTTP 403: Failed, saying GitHub is limiting requests", st.state == State::Failed && Has(st.error, "limiting"), st.error);
        st = Check(me, s.Url());
        Check("an answer that is not JSON: Failed", st.state == State::Failed && Has(st.error, "could not be read"), st.error);
        st = Check(me, s.Url());
        Check("a connection closed with no answer: Failed", st.state == State::Failed, st.error);
        const ULONGLONG t0 = GetTickCount64();
        st = Check(me, s.Url(), 700);
        const ULONGLONG took = GetTickCount64() - t0;
        Check("silence: Failed within the time allowed, not stuck", st.state == State::Failed && took < 5000, std::to_string(took) + " ms: " + st.error);
        st = Check(me, s.Url(), 3000);
        Check("an answer far larger than expected is refused", st.state == State::Failed && Has(st.error, "larger"), st.error);
        s.Stop();
        st = Check(me, s.Url(), 1000);
        Check("nothing listening: Failed", st.state == State::Failed, st.error);
        st = Check(me, L"not a url", 1000);
        Check("an address that is not one: Failed, in words", st.state == State::Failed && Has(st.error, "address"), st.error);
        Check("every state has a sentence", !DescribeStatus(Status()).empty() && Has(DescribeStatus(Status{ State::Failed, "0.4.1", "", "", "boom", 0 }), "boom"));
    }

    printf("== the worker, what it remembers, and the notice\n");
    {
        auto& cfg = ConfigManager::Instance();
        Server s;
        s.Start({ { Step::Reply, 200, Answer("v99.0.0") }, { Step::Reply, 200, Answer("v99.0.0") }, { Step::Reply, 200, Answer("v99.0.0") } });
        SetUrlForTest(s.Url().c_str());
        Check("before any check: nothing to announce, and the setting is on by default", Current().state == State::Idle && TakeNotice().empty() && cfg.GlobalGetOr<bool>("updates", "check", true));

        Tick();   // never checked and on by default: a check starts by itself
        Check("Tick starts the check when it is due (on by default)", WaitNotChecking(10000) && Current().state == State::Available && s.Served() == 1, DescribeStatus(Current()));
        const int64_t last = cfg.GlobalGetOr<int64_t>("updates", "last_check", 0);
        Check("the time of the check is remembered in the settings", last > 1700000000 && last <= static_cast<int64_t>(time(nullptr)) + 1, std::to_string(last));
        const std::string note = TakeNotice();
        Check("a newer version is announced once, with both version numbers", Has(note, "99.0.0") && Has(note, me.c_str()) && Has(note, "About"), note);
        Check("...and not again", TakeNotice().empty());
        Check("...and the announcement is remembered for the next run", cfg.GlobalGetOr<std::string>("updates", "announced", "") == "99.0.0");

        Tick();
        Sleep(400);
        Check("Tick does nothing again within a day", s.Served() == 1 && Current().state == State::Available);

        cfg.GlobalSet("updates", "last_check", static_cast<int64_t>(1));
        cfg.GlobalSet("updates", "check", false);
        Tick();
        Sleep(400);
        Check("switched off in Settings, Tick never checks, however long ago the last one was", s.Served() == 1);

        StartCheckAsync();   // "Check now" works whatever the setting is
        Check("Check now works even when the daily check is off", WaitNotChecking(10000) && s.Served() == 2 && Current().state == State::Available);
        StartCheckAsync();
        StartCheckAsync();   // asked again straight away: only one runs
        WaitNotChecking(10000);
        Sleep(300);
        Check("asking twice at once runs one check", s.Served() == 3, std::to_string(s.Served()));
        s.Stop();
        SetUrlForTest(nullptr);
    }

    printf("== the release's zip in GitHub's answer\n");
    {
        const std::string answer = std::string("{\"tag_name\":\"v0.9.9\",\"draft\":false,\"prerelease\":false,\"assets\":[") +
            "{\"name\":\"LSAddonManager-0.9.9-x64.zip\",\"size\":1234,\"digest\":\"sha256:" + std::string(64, 'a') + "\",\"browser_download_url\":\"https://evil.example/x.zip\"}," +
            "{\"name\":\"other.zip\",\"size\":5,\"digest\":\"sha256:" + std::string(64, 'b') + "\"}]}";
        const Release r = ParseLatestRelease(answer);
        Check("the zip is found by its name, with its size and SHA-256", r.ok && r.zipSize == 1234 && r.zipSha256 == std::string(64, 'a'), r.zipSha256);
        Check("its address is built from the tag, not taken from the answer",
              r.zipUrl == std::string(kReleasesPage) + "/download/v0.9.9/LSAddonManager-0.9.9-x64.zip", r.zipUrl);
        const Release bad = ParseLatestRelease("{\"tag_name\":\"v0.9.9\",\"assets\":[{\"name\":\"LSAddonManager-0.9.9-x64.zip\",\"size\":9,\"digest\":\"md5:abc\"}]}");
        Check("a digest that is not a SHA-256 is not taken", bad.ok && bad.zipSha256.empty() && !bad.zipUrl.empty());
    }

    printf("== the release notes, as plain text\n");
    {
        const std::string md = "## 0.9.8\r\n\r\n\r\nIntro with **bold** and `code`.\n- **A point.** More text.\n* star bullet\n  - nested [a link](https://evil.example/x)\n";
        const std::string plain = PlainNotes(md);
        Check("headings, bold and code marks go; bullets stay; a link keeps only its text", plain == "0.9.8\n\nIntro with bold and code.\n- A point. More text.\n- star bullet\n  - nested a link", plain);
        Check("long notes are cut at a line, saying where the rest is", Has(PlainNotes(std::string(5000, 'x') + "\n", 100), "the release page has the rest"));
        const Release r = ParseLatestRelease("{\"tag_name\":\"v0.9.9\",\"body\":\"- **New** thing\"}");
        Check("the notes come from the release's body", r.ok && r.notes == "- New thing", r.notes);
    }

    printf("== \"Don't ask again for this release\"\n");
    {
        Skip("0.9.9");
        Check("the release declined is not offered again", Skipped("0.9.9"));
        Check("a newer one is", !Skipped("0.9.10") && !Skipped(""));
    }

    printf("== old downloads cleaned up\n");
    {
        const fs::path root = dir / "downloads";
        for (const char* v : { "0.9.1", "9.9.8", "9.9.9", "not-a-version" }) { fs::create_directories(root / v); std::ofstream(root / v / "LSAddonManagerSetup.exe") << "x"; }
        SetDownloadRootForTest(root.wstring().c_str());
        CleanOldDownloads("9.9.9");
        Check("a version already installed is removed", !fs::exists(root / "0.9.1"));
        Check("another newer version than the one downloading is removed", !fs::exists(root / "9.9.8"));
        Check("the one downloading stays", fs::exists(root / "9.9.9" / "LSAddonManagerSetup.exe"));
        Check("a folder that is not a version (not ours) stays", fs::exists(root / "not-a-version"));
        CleanOldDownloads("");
        Check("with nothing downloading, a newer version stays (it may be ready to install)", fs::exists(root / "9.9.9"));
        SetDownloadRootForTest(nullptr);
    }

    printf("== downloading and checking an update (a local server; the Setup exe this build made)\n");
    {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const fs::path setup = fs::path(exe).parent_path().parent_path().parent_path().parent_path() / "installer" / "build" / "Release" / "LSAddonManagerSetup.exe";
        if (!fs::exists(setup)) printf("SKIP  (no %s: build the installer first)\n", setup.string().c_str());
        else {
            // the version Setup says it is (a build folder's Setup can be older than this test; the check under test is that the two agree)
            std::string setupVersion;
            {
                DWORD h = 0; const DWORD n = GetFileVersionInfoSizeW(setup.c_str(), &h);
                std::vector<char> info(n);
                wchar_t* v = nullptr; UINT len = 0;
                if (n && GetFileVersionInfoW(setup.c_str(), 0, n, info.data()) && VerQueryValueW(info.data(), L"\\StringFileInfo\\040904b0\\ProductVersion", reinterpret_cast<void**>(&v), &len) && len)
                    for (const wchar_t* c = v; *c; ++c) setupVersion += static_cast<char>(*c);
            }
            if (setupVersion != me) printf("note  the Setup found is %s, this build is %s: rebuild the installer to test the same version\n", setupVersion.c_str(), me.c_str());
            const fs::path pack = dir / "pack", zip = dir / ("LSAddonManager-" + setupVersion + "-x64.zip");
            fs::create_directories(pack);
            fs::copy_file(setup, pack / "LSAddonManagerSetup.exe", fs::copy_options::overwrite_existing);
            wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
            std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -a -cf \"" + zip.wstring() + L"\" -C \"" + pack.wstring() + L"\" LSAddonManagerSetup.exe";
            STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 60000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            }
            std::string bytes;
            { std::ifstream in(zip, std::ios::binary); bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()); }
            const std::string sha = AddonSecurity::ComputeSHA256(zip.wstring());
            Check("a zip holding Setup was made for the test", bytes.size() > 1000 && sha.size() == 64, std::to_string(bytes.size()));
            auto serve = [&](const std::string& expectVersion, const std::string& expectSha, const char* folderName, std::atomic<uint64_t>* done) {
                Server s; s.Start({ Step{ Step::Reply, 200, bytes } });
                const Progress p = DownloadAndCheck(expectVersion, s.Url("/download/zip"), expectSha, bytes.size(), (dir / folderName).wstring(), nullptr, done, nullptr);
                s.Stop();
                return p;
            };
            std::atomic<uint64_t> done{ 0 };
            const Progress good = serve(setupVersion, sha, "good", &done);
            Check("the download is checked and Setup unpacked: ready", good.step == Phase::Ready && fs::exists(good.setup), good.error);
            Check("...its progress counted every byte", done == bytes.size(), std::to_string(done.load()) + " of " + std::to_string(bytes.size()));
            const Progress wrongSha = serve(setupVersion, std::string(64, 'c'), "badsha", nullptr);
            Check("a download that does not match GitHub's SHA-256 is refused", wrongSha.step == Phase::Failed && Has(wrongSha.error, "SHA-256"), wrongSha.error);
            const Progress wrongVersion = serve("9.9.9", sha, "badversion", nullptr);
            Check("a Setup of another version than the release's is refused", wrongVersion.step == Phase::Failed && Has(wrongVersion.error, "is not LS Addon Manager"), wrongVersion.error);
            std::atomic<bool> cancel{ true };
            Server s; s.Start({ Step{ Step::Reply, 200, bytes } });
            const Progress cancelled = DownloadAndCheck(setupVersion, s.Url("/download/zip"), sha, bytes.size(), (dir / "cancelled").wstring(), &cancel, nullptr, nullptr);
            s.Stop();
            Check("a cancelled download stops and leaves nothing ready", cancelled.step == Phase::Failed && cancelled.error == "cancelled", cancelled.error);
        }
    }

    if (live) {
        printf("== the real GitHub\n");
        const Status st = Check(me, kLatestReleaseUrl, 15000);
        printf("  %s\n", DescribeStatus(st).c_str());
        Check("the real answer arrived and was read (needs the internet)", st.state == State::UpToDate || st.state == State::Available, st.error);
    }

    Logger::Instance().Shutdown();   // it holds its log file open, which would keep the folder from being removed
    std::error_code ec;
    fs::remove_all(dir, ec);
    printf("\n%s (%d failed)\n", g_failed ? "UPDATE TEST FAILED" : "UPDATE TEST PASSED", g_failed);
    return g_failed ? 1 : 0;
}
