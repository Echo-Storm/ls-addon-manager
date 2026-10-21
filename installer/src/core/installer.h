#pragma once
// Installing LS Addon Manager into a Lossless Scaling folder, and taking it out again.
//
// Every change is journalled. If anything fails part-way, the journal is played backwards and the folder is put back as it was; a folder that was
// left as it was is reported as such. Files that are replaced are copied to <Lossless Scaling>\backups\installer-<time>\ first, and nothing is deleted:
// what leaves the folder goes there. The person's settings (addons\config.json), their other addons and addons\.removed are never touched.
#include "state.h"
#include <string>
#include <vector>

namespace setup {

struct PayloadInfo {
    bool ok = false;
    std::string version;             // the version of the Lossless.dll it carries ("0.4.1")
    std::string error;
};
// A payload is a folder laid out like the release zip: Lossless.dll (ours), manager-icon.ico, manager-icon.png and an addons folder.
PayloadInfo CheckPayload(const std::wstring& payloadDir);

struct Result {
    bool ok = false;
    std::string message;             // a sentence for the person
    std::vector<std::string> log;    // what was done, step by step
    std::wstring backupDir;          // where replaced files went (empty when nothing had to be saved)
    bool rolledBack = false;         // it failed part-way and the folder was put back as it was
    std::vector<std::string> kept;   // addon files the person had changed, which were left as they are (paths under the Lossless Scaling folder)
};

// Install, update or repair, whichever the folder needs. Refuses while Lossless Scaling is running, and for a folder in a state it cannot mend.
Result Install(const std::wstring& lsDir, const std::wstring& payloadDir);

// Tests only: name every backups folder with this stamp instead of the time (empty: the time again). A folder that already exists gets a number added.
void SetBackupStampForTest(const std::wstring& stamp);

// Put Lossless Scaling's own Lossless.dll back. The addons and settings stay unless `removeAddons` (then the addons folder is moved to the backups).
Result Uninstall(const std::wstring& lsDir, bool removeAddons);

} // namespace setup
