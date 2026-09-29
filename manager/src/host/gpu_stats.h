#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace eam {

// GPU load, power, clocks, temperature and memory of the first NVIDIA GPU, read through NVML. Without NVML (an AMD or Intel card) the load and
// the memory in use come from Windows' own counters (the Task Manager's), the name and memory size from DXGI, for the card with the most
// memory; power, clocks and temperature are then not reported (the has* flags say which are). Sampling runs on its own thread, only
// while something has asked for it recently (Wanted()), so an idle manager costs nothing.
class GpuStats {
public:
    static GpuStats& Instance();
    // If the process ends with the sampler still running (Shutdown was not called), let go of it: destroying a joinable std::thread calls
    // std::terminate, which would crash Lossless Scaling at exit.
    ~GpuStats() { if (m_thread.joinable()) m_thread.detach(); }

    struct Snapshot {
        bool ok = false;               // NVML loaded and a GPU answered
        std::string name, driver, why; // why: the reason it is unavailable
        int deviceCount = 0;
        unsigned utilGpu = 0, utilMem = 0;          // percent
        unsigned clockGraphics = 0, clockMem = 0;   // MHz
        unsigned tempC = 0;
        double powerW = 0, powerLimitW = 0;
        uint64_t vramUsedMB = 0, vramTotalMB = 0;
        uint64_t throttle = 0;         // NVML clocks-throttle-reasons bitmask
        double ageSeconds = 1e9;       // time since this snapshot was taken
        bool viaCounters = false;      // from Windows' counters, not NVML: only the load and the memory are reported
        bool hasPower = false, hasClocks = false, hasTemp = false, hasThrottle = false;   // which of the readings above are real
    };

    // For tests and the probe: use Windows' counters even where NVML works (set before the first sample).
    static void PreferCounters(bool on);

    // Call every frame the Performance tab is showing: starts the sampler on first use and keeps it running.
    void Wanted();
    Snapshot Get() const;
    void Shutdown();                   // stop the thread (process exit)

    // Human-readable names for the set bits of a throttle-reason mask ("power limit", "thermal", ...), most relevant first.
    static std::string ThrottleText(uint64_t mask);

    // For tests: take one sample on the calling thread.
    bool SampleOnce();
    // For offline previews: pretend the GPU reported this (and never sample the real one from then on).
    void InjectForPreview(const Snapshot& s);

private:
    GpuStats() = default;
    void Run();
    bool Init();
    bool InitNvml(void* lib, std::string& why);
    bool InitCounters(std::string* why);
    bool SampleCounters(Snapshot& s);

    mutable std::mutex m_mutex;
    Snapshot m_snap;
    std::atomic<bool> m_started{ false }, m_stop{ false };
    std::atomic<int64_t> m_wantedAtMs{ 0 };
    std::thread m_thread;
    void* m_lib = nullptr;   // HMODULE of nvml.dll
    void* m_dev = nullptr;   // nvmlDevice_t
    bool m_inited = false;
    bool m_counters = false;   // sampling Windows' counters instead of NVML
    int64_t m_takenAtMs = 0;
};

} // namespace eam
