// Offline test of the frame trace (src/addon/frame_trace.cpp): events from several threads are all kept while they fit, the newest win once the ring wraps, and the export is a
// CSV the analyser can read (tools/analyze_frame_trace.py).
#include "addon/frame_trace.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static int g_failed = 0;
static void Check(const char* what, bool ok, const std::string& detail = "") {
    printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  (", detail.empty() ? "" : (detail + ")").c_str());
    if (!ok) ++g_failed;
}

struct Parsed { long long clock = 0; std::vector<std::vector<std::string>> rows; bool header = false; };
static Parsed Read(const std::wstring& path) {
    Parsed p; std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("#clock,", 0) == 0) { p.clock = atoll(line.c_str() + 7); continue; }
        if (line[0] == '#') continue;
        if (line.rfind("t_us,", 0) == 0) { p.header = true; continue; }
        std::vector<std::string> cols; std::stringstream ss(line); std::string c;
        while (std::getline(ss, c, ',')) cols.push_back(c);
        p.rows.push_back(cols);
    }
    return p;
}

int main() {
    using namespace nr::trace;
    wchar_t dir[MAX_PATH]; GetTempPathW(MAX_PATH, dir);
    const std::wstring path = std::wstring(dir) + L"nr_trace_test.csv";

    // a few events from four threads at once
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) threads.emplace_back([t] { for (int i = 0; i < 1000; ++i) Add(t % 2 ? kPresent : kModel, t, i, t * 10000 + i); });
        for (auto& th : threads) th.join();
        std::string error;
        Check("the export is written", Export(path, &error), error);
        const Parsed p = Read(path);
        Check("...with a header and the clock", p.header && p.clock > 0);
        Check("every event of four threads is kept (none lost, none doubled)", p.rows.size() == 4000, std::to_string(p.rows.size()));
        bool ordered = true; long long last = -1; std::vector<int> perThread(4, 0);
        for (const auto& r : p.rows) { const long long us = atoll(r[0].c_str()); if (us < last - 2000) ordered = false; if (us > last) last = us; const int t = atoi(r[2].c_str()); if (t >= 0 && t < 4) ++perThread[t]; }
        Check("...in time order (events from different threads may be a few microseconds apart in the file; the analyser sorts)", ordered);
        Check("...1000 from each thread", perThread[0] == 1000 && perThread[1] == 1000 && perThread[2] == 1000 && perThread[3] == 1000);
        Check("the kinds are named", p.rows[0][1] == "present" || p.rows[0][1] == "model");
    }

    // the ring wraps: the newest kCapacity events stay
    {
        const uint32_t extra = kCapacity + kCapacity / 2;
        for (uint32_t i = 0; i < extra; ++i) Add(kMark, 7, 0, static_cast<int32_t>(i));
        std::string error;
        Check("the export after the ring has wrapped is written", Export(path, &error), error);
        const Parsed p = Read(path);
        Check("...it holds no more than the ring's size", p.rows.size() <= kCapacity, std::to_string(p.rows.size()));
        Check("...and it is nearly full (events in the middle of being overwritten are skipped, nothing else)", p.rows.size() > kCapacity - 8, std::to_string(p.rows.size()));
        bool newest = !p.rows.empty() && atoi(p.rows.back()[4].c_str()) == static_cast<int>(extra - 1);
        Check("...ending with the newest event", newest, p.rows.empty() ? "empty" : p.rows.back()[4]);
        bool oldestIsRecent = !p.rows.empty() && atoi(p.rows.front()[4].c_str()) > static_cast<int>(extra - kCapacity - 10) - 1 - 4000;
        Check("...and the oldest kept is from the newest kCapacity events", oldestIsRecent);
    }

    // a path that cannot be written is reported, not a crash
    {
        std::string error;
        Check("an unwritable path is reported", !Export(L"Z:\\no\\such\\folder\\trace.csv", &error) && !error.empty(), error);
    }

    printf("\n%s\n", g_failed ? "FRAME TRACE TEST FAILED" : "FRAME TRACE TEST PASSED");
    DeleteFileW(path.c_str());
    return g_failed ? 1 : 0;
}
