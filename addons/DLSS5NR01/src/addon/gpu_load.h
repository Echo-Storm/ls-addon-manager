// How busy the graphics card is, for the "Keep the graphics card under N %" setting of auto quality. Read through NVML (nvml.dll comes with the NVIDIA driver and is
// loaded by name, nothing is linked), at most once a second, from the thread that asks: one cheap call a second, so no thread of its own to stop at exit.
// On a machine with several NVIDIA cards the one whose name is the card Lossless Scaling runs on is read (the first when no name fits).
#pragma once
#include <cstdint>
#include <string>

namespace nr {

class GpuLoad {
public:
    // The percent of the last sample period the card was busy (0..100), or 0 when it is not known (no NVML, no card answered).
    unsigned Percent(uint64_t nowMs, const std::string& cardName);
    bool Available() const { return m_ok; }
    // What went wrong, when it is not available.
    const std::string& Why() const { return m_why; }

private:
    bool Init(const std::string& cardName);
    bool m_tried = false, m_ok = false;
    std::string m_why, m_cardName;
    void* m_lib = nullptr;
    void* m_dev = nullptr;
    int (*m_getUtil)(void*, void*) = nullptr;
    uint64_t m_lastAt = 0;
    unsigned m_last = 0;
};

}   // namespace nr
