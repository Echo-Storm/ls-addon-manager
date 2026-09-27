#pragma once
// Is there a newer release? Asks GitHub for the latest release of this project, compares the version numbers, and says what it found. That is all it
// does: it never downloads or installs anything. It is on by default (once a day) and can be turned off in Settings > Updates; "Check now" always works.
// It is the manager's only network access. The request is an ordinary HTTPS GET to api.github.com; GitHub sees the IP address, and the program's name
// and version in the User-Agent, as with any download.
#include <cstdint>
#include <string>

namespace eam {
namespace update {

// ---- versions
struct Version {
    int major = 0, minor = 0, patch = 0;
    bool ok = false;
};
Version ParseVersion(const std::string& text);   // "v0.4.1", "0.4.1", "0.4" and "0.5.0-beta.1" (the suffix is ignored); not ok for anything else
int Compare(const Version& a, const Version& b); // -1, 0 or 1; numbers are compared as numbers, so 0.10.0 is newer than 0.9.0

// ---- the answer from GitHub (the "latest release" of a repository)
struct Release {
    bool ok = false;               // a usable answer: a tag that reads as a version
    bool draft = false, prerelease = false;
    std::string tag;               // "v0.4.1"
    std::string version;           // "0.4.1"
    std::string url;               // this project's page for that release; built here, never taken from the answer
    // the release's zip, LSAddonManager-<version>-x64.zip: its address is built here (never taken from the answer); GitHub's answer gives its
    // size and SHA-256 ("digest"), which the download is checked against (empty / 0 when the answer has no such file)
    std::string zipName, zipUrl, zipSha256;
    uint64_t zipSize = 0;
    std::string notes;             // the release notes ("body"), as plain text (PlainNotes), for the update window
};
Release ParseLatestRelease(const std::string& json);
// Release notes written in Markdown, as plain text to show: "## " headings and "**", "`" and "_" marks dropped, "- " bullets kept, links
// reduced to their text (nothing in them can be opened). Cut at a line near maxChars, with a note that the release page has the rest.
std::string PlainNotes(const std::string& markdown, size_t maxChars = 3000);

// ---- when to ask
// Once a day while the check is switched on. A last-check time in the future (the clock was set back) counts as never, so the check cannot be stuck.
bool ShouldCheckNow(bool enabled, int64_t lastCheck, int64_t now, int64_t intervalSeconds = 24 * 3600);

// ---- fetching
struct FetchResult {
    bool ok = false;               // an answer arrived (of any HTTP status)
    int status = 0;
    std::string body;
    std::string error;             // in words, when nothing arrived
};
// An HTTP(S) GET that gives up after `timeoutMs` for any single step and after about three times that in all, and refuses an answer over `maxBytes`.
FetchResult HttpGet(const std::wstring& url, unsigned timeoutMs, size_t maxBytes);

// ---- the check
enum class State { Idle, Checking, UpToDate, Available, Failed };
struct Status {
    State state = State::Idle;
    std::string current;           // the running version
    std::string latest;            // the newest release found
    std::string url;               // its page (only when Available)
    std::string error;             // why it failed, in words
    int64_t checkedAt = 0;         // when, as seconds since 1970
    std::string zipUrl, zipSha256; uint64_t zipSize = 0;   // its zip (only when Available and the release has one): see Release
    std::string notes;                                      // its release notes, as plain text (only when Available)
};

extern const wchar_t* const kLatestReleaseUrl;   // https://api.github.com/repos/Echo-Storm/ls-addon-manager/releases/latest
extern const char* const kReleasesPage;           // https://github.com/Echo-Storm/ls-addon-manager/releases

Status Check(const std::string& currentVersion, const std::wstring& url, unsigned timeoutMs = 10000);   // blocking

// ---- the running manager's check: on a worker thread, with its result kept here and its time remembered in the settings
void StartCheckAsync();          // does nothing if one is already running
Status Current();
void SetUrlForTest(const wchar_t* url);   // tests point the check at a server of their own; null goes back to GitHub
void SetStatusForTest(const Status& st);  // the UI preview: a result as if a check had found it

// Called about once a minute: starts the daily check when it is due. Reads Settings > Updates (updates.check, on unless turned off; updates.last_check).
void Tick();
// A message to show once when a version newer than the one last announced has been found ("" otherwise).
std::string TakeNotice();

std::string DescribeStatus(const Status& s);   // one line for Settings and About: "You have the latest version (0.4.1)." and so on

} // namespace update
} // namespace eam
