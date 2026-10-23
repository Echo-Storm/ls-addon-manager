#pragma once
#include <filesystem>
#include <functional>
#include <string>

namespace eam {

struct DiagResult { bool ok = false; std::string message; std::filesystem::path zip; };

// Bundles what is needed to look into a problem into one zip in `outDir`: every .log under <lsDir>\logs and under each addon's folder
// (a log bigger than 3 MB is cut to its newest 3 MB), <lsDir>\addons\config.json, and `summary` as info.txt. Nothing is uploaded or sent
// anywhere; the user decides who gets the file. Uses Windows' own tar.exe to write the zip.
// `extras` (may be empty) is called with the folder that becomes the zip, to put more files in it (the report's system information, history, metrics and the addons' traces); `prefix` names the zip.
DiagResult CreateDiagnosticsZip(const std::filesystem::path& lsDir, const std::string& summary, const std::filesystem::path& outDir,
                                const std::function<void(const std::filesystem::path&)>& extras = {}, const std::wstring& prefix = L"LSAddonManager-diagnostics");

} // namespace eam
