// A short memory of what happened, for a report: what Lossless Scaling's window sent it (ApplySettings), scaling starts and stops, addons loaded and unloaded, an addon's error. A
// ring of the newest entries with the time of day, written from any thread.
#pragma once
#include <string>

namespace eam::history {

void Add(const char* kind, const std::string& text);
std::string Dump();   // one line each, oldest first: "HH:MM:SS.mmm  kind  text"

} // namespace eam::history
