// The report: everything that is useful to look into a problem, in one zip (the Super log feature saves it by a hotkey, a button, or by itself when an addon reports an error). It is the
// diagnostics zip (logs, settings, a summary) plus: system.txt (Windows, graphics cards and drivers, displays, Lossless Scaling's version, the addons' versions and files), history.txt
// (what Lossless Scaling's window sent it and when, scaling starts and stops, addons loaded, errors), metrics.csv (the last ten minutes of every series the addons publish) and what each
// addon adds when told a report is being made (its frame trace: every present, upscaler pass and model run). Nothing is uploaded.
#pragma once
#include "diagnostics.h"
#include <string>

namespace eam {

DiagResult MakeReport(const std::filesystem::path& lsDir, const std::string& reason, const std::filesystem::path& outDir);

} // namespace eam
