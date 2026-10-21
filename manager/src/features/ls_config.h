#pragma once
#include <string>

namespace eam {
namespace features {
namespace lsconfig {

// "Lossless Scaling's hidden settings": an editor for the settings file beside Lossless Scaling (config.ini) that its window does not show: the look and place of its frame rate
// counter, and a few expert values. The file is Lossless Scaling's own and is read when scaling starts, so a change applies from the next time. The manager only edits the lines
// it is asked to; the rest of the file stays as it is, and the first change keeps a copy (config.ini.bak-manager).
constexpr const char* kId = "LsConfig";

void RenderOptions();

// The text of an INI file with `key` in `section` set (added when missing, the section too); every other line is kept as it is. (Public for the tests.)
std::string IniSet(const std::string& text, const std::string& section, const std::string& key, const std::string& value);
// The value of `key` in `section` (trimmed), or `fallback`.
std::string IniGet(const std::string& text, const std::string& section, const std::string& key, const std::string& fallback);

} // namespace lsconfig
} // namespace features
} // namespace eam
