#pragma once
// Updating from the manager, when the person asks for it (the update check, update_check.h, only finds a newer release).
//
// 1. The release's zip is downloaded into %TEMP%\LSAddonManager-update\<version> (its address built from the release's tag, never taken from
//    GitHub's answer), with its progress, and it can be cancelled.
// 2. It is checked: its size and SHA-256 against GitHub's ("digest"), then LSAddonManagerSetup.exe is unpacked from it (Windows' own tar.exe)
//    and must say it is "LS Addon Manager Setup" of that very version.
// 3. That Setup is started with --update-when-closed: it waits for Lossless Scaling to close (the manager lives inside it, so nothing can be
//    replaced while it runs), updates the folder with its usual backups and rollback, and starts Lossless Scaling again. As administrator only
//    when the folder needs it.
// "Don't ask again for this release" is remembered in the settings (updates.skipped); a newer release asks again.
#include "update_check.h"
#include <atomic>
#include <cstdint>
#include <string>

namespace eam {
namespace update {

enum class Phase { Idle, Downloading, Checking, Ready, Failed, Handed };
struct Progress {
    Phase step = Phase::Idle;
    std::string version;
    uint64_t done = 0, total = 0;   // bytes, while downloading (total 0: not known)
    std::string error;              // in words, when Failed
    std::wstring setup;             // the checked Setup exe, when Ready
};

void StartDownload(const Status& available);   // Available with a zip; one already running (or ready for this version) is left as it is
void CancelDownload();
Progress CurrentProgress();
void ForgetDownload();                         // back to Idle (after a failure, or when the person closes the offer)

// Starts the checked Setup on the Lossless Scaling folder; false with the reason if it could not be started.
bool StartSetup(const std::wstring& lsDir, std::string& error);

// The updater's own downloads in %TEMP%\LSAddonManager-update\<version> (about 110 MB each): the folders of versions this build already
// is or is newer than, and of any other version than keep (the one being downloaded now; empty: none). Called at a download's start and
// once a day from Tick. A folder in use (a Setup still running from it) is simply left for next time.
void CleanOldDownloads(const std::string& keep);
void SetDownloadRootForTest(const wchar_t* folder);   // the folder CleanOldDownloads looks in (null: %TEMP%\LSAddonManager-update)

bool Skipped(const std::string& version);      // "Don't ask again for this release"
void Skip(const std::string& version);

// The whole of steps 1 and 2, blocking: for the running manager's worker and for the tests (a local address, a folder of their own).
Progress DownloadAndCheck(const std::string& version, const std::wstring& url, const std::string& sha256, uint64_t size, const std::wstring& folder,
                          const std::atomic<bool>* cancel, std::atomic<uint64_t>* done, std::atomic<uint64_t>* total);
void SetDownloadUrlForTest(const wchar_t* url);   // the zip's address for the running manager's download (null: the release's own)

} // namespace update
} // namespace eam
