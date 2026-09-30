#include "addon/frame_trace.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

namespace nr::trace {

namespace {

struct Event {
    std::atomic<uint64_t> seq{ 0 };   // 1 + the event's number once its fields are written: an export skips a slot being written
    int64_t qpc = 0;
    int32_t a = 0, b = 0, c = 0;
    uint8_t kind = 0;
};

Event g_ring[kCapacity];
std::atomic<uint64_t> g_next{ 0 };
std::atomic<int64_t> g_start{ 0 };
std::mutex g_exportMutex;

const char* KindName(uint8_t k) {
    switch (k) {
    case kTap: return "tap";
    case kPresent: return "present";
    case kModel: return "model";
    case kUpscale: return "upscale";
    case kAuto: return "auto";
    case kHotkey: return "hotkey";
    default: return "mark";
    }
}

} // namespace

void Add(Kind kind, int32_t a, int32_t b, int32_t c) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    int64_t expected = 0; g_start.compare_exchange_strong(expected, now.QuadPart);   // the first event starts the clock
    const uint64_t n = g_next.fetch_add(1, std::memory_order_relaxed);
    Event& e = g_ring[n & (kCapacity - 1)];
    e.seq.store(0, std::memory_order_release);
    e.qpc = now.QuadPart; e.kind = kind; e.a = a; e.b = b; e.c = c;
    e.seq.store(n + 1, std::memory_order_release);
}

bool Export(const std::wstring& path, std::string* error) {
    std::lock_guard<std::mutex> lock(g_exportMutex);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) { if (error) *error = "the file cannot be written"; return false; }
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    const uint64_t next = g_next.load(std::memory_order_acquire);
    const uint64_t first = next > kCapacity ? next - kCapacity : 0;
    const int64_t start = g_start.load();
    fprintf(f, "#clock,%lld,%lld\n", static_cast<long long>(freq.QuadPart), static_cast<long long>(start));
    fprintf(f, "#events,%llu,kept %llu\n", static_cast<unsigned long long>(next), static_cast<unsigned long long>(next - first));
    fprintf(f, "t_us,kind,a,b,c\n");
    for (uint64_t n = first; n < next; ++n) {
        const Event& e = g_ring[n & (kCapacity - 1)];
        if (e.seq.load(std::memory_order_acquire) != n + 1) continue;   // overwritten or being written
        const long long us = static_cast<long long>((e.qpc - start) * 1000000 / freq.QuadPart);
        fprintf(f, "%lld,%s,%d,%d,%d\n", us, KindName(e.kind), e.a, e.b, e.c);
    }
    const bool ok = fclose(f) == 0;
    if (!ok && error) *error = "the file could not be completed";
    return ok;
}

} // namespace nr::trace
