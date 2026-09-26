#pragma once
// The offer to update, as a small window over the manager when the update check finds a newer release: Download and install, Not now,
// or Don't ask again for this release. Then the download's progress (with Cancel), and once it is checked: Install now (Setup waits for
// Lossless Scaling to close, updates, and starts it again) or Later. See update/updater.h.

namespace eam {
namespace widgets {

void UpdateOffer();        // every frame, inside the main window: opens the offer by itself for a release not declined
void OpenUpdateOffer();    // the About tab's button: the offer again, even for a release declined

} // namespace widgets
} // namespace eam
