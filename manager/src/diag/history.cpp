#include "history.h"
#include <windows.h>
#include <cstdio>
#include <mutex>
#include <vector>

namespace eam::history {

namespace {
constexpr size_t kKeep = 600;
struct Entry { SYSTEMTIME at; std::string kind, text; };
std::mutex g_mutex;
std::vector<Entry> g_ring; size_t g_head = 0;
}

void Add(const char* kind, const std::string& text) {
    Entry e; GetLocalTime(&e.at); e.kind = kind ? kind : ""; e.text = text;
    if (e.text.size() > 600) e.text.resize(600);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_ring.size() < kKeep) g_ring.push_back(std::move(e)); else { g_ring[g_head] = std::move(e); g_head = (g_head + 1) % kKeep; }
}

std::string Dump() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string out; char t[48];
    for (size_t i = 0; i < g_ring.size(); ++i) {
        const Entry& e = g_ring[(g_head + i) % g_ring.size()];
        snprintf(t, sizeof t, "%02d:%02d:%02d.%03d  ", e.at.wHour, e.at.wMinute, e.at.wSecond, e.at.wMilliseconds);
        out += t; out += e.kind; out += "  "; out += e.text; out += "\n";
    }
    return out;
}

} // namespace eam::history
