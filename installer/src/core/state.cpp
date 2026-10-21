#include "state.h"
#include "fileinfo.h"
#include "lsfolder.h"
#include <windows.h>
#include <tlhelp32.h>
#include <cstdlib>

namespace setup {

DllInfo InspectDll(const std::wstring& path) {
    DllInfo info;
    uint64_t size = 0;
    if (!FileSizeOf(path, size)) return info;
    info.exists = true;
    info.size = size;
    const VersionResource v = ReadVersionResource(path);
    info.version = v.fileVersion;
    if (v.ok && (v.product == "Addon Manager for Lossless Scaling" || v.product == "Echo Addon Manager")) info.kind = DllKind::Ours;   // the name since 0.9.1, and before
    else if (v.ok && v.product == "Lossless Scaling") info.kind = DllKind::Original;
    else info.kind = DllKind::Unknown;
    // Releases up to 0.4.1 carry no version resource. They (and the proxy this project started from) do carry the name of their own log file, which
    // Lossless Scaling's DLL does not, so an unrecognised DLL with one of those names in it is an earlier one of ours (Echo Addon Manager, as it
    // was called then, or the proxy it began as).
    if (info.kind == DllKind::Unknown && (FileContainsText(path, "EchoAddonManager.log") || FileContainsText(path, "LosslessProxy.log"))) {
        info.kind = DllKind::Ours;
        info.legacy = true;
        info.version.clear();   // unknown: older than any release that has a version resource
    }
    return info;
}

bool LosslessScalingRunning(const std::wstring& dir) {
    bool running = false;
    if (dir.empty()) return false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    const std::wstring here = CanonicalPath(dir);   // the folder however it was written: trailing or forward slashes, "..", a short 8.3 name, a link
    PROCESSENTRY32W p = {};
    p.dwSize = sizeof p;
    for (BOOL ok = Process32FirstW(snap, &p); ok && !running; ok = Process32NextW(snap, &p)) {
        if (_wcsicmp(p.szExeFile, L"LosslessScaling.exe") != 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.th32ProcessID);
        if (!h) { running = true; continue; }   // one we cannot look at: assume it is the one
        wchar_t path[MAX_PATH * 2] = {};
        DWORD n = static_cast<DWORD>(sizeof path / sizeof path[0]);
        if (QueryFullProcessImageNameW(h, 0, path, &n)) {
            const std::wstring image = std::wstring(path);
            const size_t at = image.find_last_of(L"\\/");
            if (at != std::wstring::npos && CanonicalPath(image.substr(0, at)) == here) running = true;
        } else {
            running = true;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return running;
}

State Inspect(const std::wstring& dir) {
    State s;
    s.dir = dir;
    s.lossless = InspectDll(JoinPath(dir, L"Lossless.dll"));
    s.original = InspectDll(JoinPath(dir, L"Lossless_original.dll"));
    s.running = LosslessScalingRunning(dir);
    if (!Exists(JoinPath(dir, L"LosslessScaling.exe"))) { s.situation = Situation::NotLosslessScaling; return s; }

    const DllKind a = s.lossless.exists ? s.lossless.kind : DllKind::Missing;
    const DllKind b = s.original.exists ? s.original.kind : DllKind::Missing;
    if (a == DllKind::Original && b == DllKind::Missing) s.situation = Situation::NotInstalled;
    else if (a == DllKind::Ours && b == DllKind::Original) s.situation = Situation::Installed;
    else if (a == DllKind::Original && b != DllKind::Missing) s.situation = Situation::AfterLsUpdate;   // whatever the stale copy is
    else if (a == DllKind::Ours && b == DllKind::Missing) s.situation = Situation::NoOriginal;
    else if (a == DllKind::Ours && b == DllKind::Ours) s.situation = Situation::BothOurs;
    else s.situation = Situation::Unrecognised;

    if (a == DllKind::Original) s.lsVersion = s.lossless.version;
    else if (b == DllKind::Original) s.lsVersion = s.original.version;
    if (a == DllKind::Ours) s.installedVersion = s.lossless.version;
    return s;
}

namespace {
void Numbers(const std::string& v, long out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = 0;
    int part = 0;
    size_t i = 0;
    while (i < v.size() && part < 4) {
        if (isdigit(static_cast<unsigned char>(v[i]))) {
            long n = 0;
            while (i < v.size() && isdigit(static_cast<unsigned char>(v[i]))) { n = n * 10 + (v[i] - '0'); ++i; }
            out[part++] = n;
        } else if (v[i] == '.') {
            ++i;
        } else {
            break;
        }
    }
}
}

int CompareVersions(const std::string& a, const std::string& b) {
    long x[4], y[4];
    Numbers(a, x);
    Numbers(b, y);
    for (int i = 0; i < 4; ++i) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

Advice Advise(const State& s, const std::string& payloadVersion) {
    Advice a;
    const std::string ls = s.lsVersion.empty() ? std::string("Lossless Scaling") : "Lossless Scaling " + s.lsVersion;
    switch (s.situation) {
    case Situation::NotLosslessScaling:
        a.headline = "Lossless Scaling was not found in this folder.";
        a.detail = "Pick the folder that holds LosslessScaling.exe.";
        a.blocked = true;
        a.blockedReason = a.headline + " " + a.detail;   // what is wrong, then what to do
        return a;
    case Situation::NotInstalled:
        a.headline = "LS Addon Manager is not installed here (" + ls + " found).";
        a.action = Action::Install;
        a.actionLabel = "Install";
        break;
    case Situation::Installed: {
        const int cmp = payloadVersion.empty() ? 0 : CompareVersions(s.installedVersion, payloadVersion);
        a.canUninstall = true;
        if (cmp < 0) {
            a.headline = (s.installedVersion.empty() ? std::string("An earlier LS Addon Manager is installed.") : "LS Addon Manager " + s.installedVersion + " is installed.") + " " + payloadVersion + " is available.";
            a.action = Action::Update;
            a.actionLabel = "Update to " + payloadVersion;
        } else if (cmp == 0) {
            a.headline = (s.installedVersion.empty() ? std::string("An earlier LS Addon Manager") : "LS Addon Manager " + s.installedVersion) + " is installed" + (s.installedVersion.empty() ? "." : " and up to date.");
            a.detail = "You can reinstall it to put its files back as they came (a file you changed is left as it is: delete it first to get ours).";
            a.action = Action::Reinstall;
            a.actionLabel = "Reinstall";
        } else {
            a.headline = "LS Addon Manager " + s.installedVersion + " is installed, which is newer than this installer (" + payloadVersion + ").";
            a.detail = "Nothing needs doing.";
            a.action = Action::None;
        }
        break;
    }
    case Situation::AfterLsUpdate:
        a.headline = "Lossless Scaling was updated and put its own files back over LS Addon Manager.";
        a.detail = "Repair keeps the new Lossless Scaling files and puts LS Addon Manager in front of them again.";
        a.action = Action::Repair;
        a.actionLabel = "Repair";
        a.canUninstall = true;
        break;
    case Situation::NoOriginal:
        a.headline = "LS Addon Manager is here, but Lossless_original.dll is missing, so Lossless Scaling cannot start.";
        a.detail = "Only Lossless Scaling's own file can fix that: verify its files in Steam or reinstall it, then run this again.";
        a.blocked = true;
        a.blockedReason = a.headline + " " + a.detail;   // what is wrong, then what to do
        return a;
    case Situation::BothOurs:
        a.headline = "Both Lossless.dll and Lossless_original.dll here are LS Addon Manager's.";
        a.detail = "Lossless Scaling's own file is not in this folder. Verify its files in Steam or reinstall it, then run this again.";
        a.blocked = true;
        a.blockedReason = a.headline + " " + a.detail;   // what is wrong, then what to do
        return a;
    case Situation::Unrecognised:
        a.headline = "The Lossless.dll in this folder is not one this installer knows.";
        a.detail = "It is neither Lossless Scaling's nor LS Addon Manager's, so nothing was changed.";
        a.blocked = true;
        a.blockedReason = a.headline + " " + a.detail;   // what is wrong, then what to do
        return a;
    }
    if (s.running && a.action != Action::None) {
        a.blocked = true;
        a.blockedReason = "Close Lossless Scaling first: it keeps these files open.";
    }
    return a;
}

} // namespace setup
