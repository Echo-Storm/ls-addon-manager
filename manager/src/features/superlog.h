#pragma once
#include <string>

namespace eam {
namespace features {
namespace superlog {

// Super log: everything that helps look into a problem, one press away. While it is on the manager's log is at Debug (what each shader asked for, each pass), and a report can be saved by the
// hotkey (Ctrl+Shift+F12), by the button, or by itself when an addon reports an error (at most one every five minutes): one zip in <Lossless Scaling folder>\reports with the logs, the settings,
// the system (cards, drivers, displays, versions), what Lossless Scaling's window sent and when, the last ten minutes of every series and each addon's frame trace (diag/report.h). Nothing is
// uploaded. The newest ten reports are kept.
inline constexpr const char* kId = "SuperLog";   // the settings section and the id of the on/off switch

void Start();                 // the switch is on
void Stop();                  // off
void RenderOptions();
std::string Status();         // "Report saved: ...", or empty

// An addon reported an error through its status line (IHost::SetStatus, level 3): saves a report by itself when the feature is on and the setting allows it.
void NotifyProblem(const std::string& addon, const std::string& text);

} // namespace superlog
} // namespace features
} // namespace eam
