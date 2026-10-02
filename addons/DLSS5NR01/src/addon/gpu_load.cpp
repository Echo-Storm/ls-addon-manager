#include "addon/gpu_load.h"
#include <windows.h>

namespace nr {

namespace {
struct NvmlUtil { unsigned gpu, memory; };
using NvmlReturn = int;   // 0 = success
}

bool GpuLoad::Init(const std::string& cardName) {
    m_tried = true; m_cardName = cardName;
    HMODULE lib = LoadLibraryW(L"nvml.dll");
    if (!lib) lib = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    if (!lib) { m_why = "nvml.dll was not found (it comes with the NVIDIA driver)"; return false; }
    auto init = reinterpret_cast<NvmlReturn (*)()>(GetProcAddress(lib, "nvmlInit_v2"));
    auto count = reinterpret_cast<NvmlReturn (*)(unsigned*)>(GetProcAddress(lib, "nvmlDeviceGetCount_v2"));
    auto handle = reinterpret_cast<NvmlReturn (*)(unsigned, void**)>(GetProcAddress(lib, "nvmlDeviceGetHandleByIndex_v2"));
    auto name = reinterpret_cast<NvmlReturn (*)(void*, char*, unsigned)>(GetProcAddress(lib, "nvmlDeviceGetName"));
    auto util = reinterpret_cast<NvmlReturn (*)(void*, NvmlUtil*)>(GetProcAddress(lib, "nvmlDeviceGetUtilizationRates"));
    if (!init || !count || !handle || !util) { m_why = "nvml.dll does not have the functions that read the load"; return false; }
    if (init() != 0) { m_why = "NVML could not start"; return false; }
    unsigned n = 0;
    if (count(&n) != 0 || n == 0) { m_why = "NVML found no card"; return false; }
    void* chosen = nullptr;
    for (unsigned i = 0; i < n; ++i) {
        void* h = nullptr;
        if (handle(i, &h) != 0 || !h) continue;
        if (!chosen) chosen = h;   // the first, when no name fits
        char text[96] = {};
        if (name && name(h, text, sizeof text) == 0 && !cardName.empty() && cardName.find(text) != std::string::npos) { chosen = h; break; }
    }
    if (!chosen) { m_why = "NVML gave no card"; return false; }
    m_lib = lib; m_dev = chosen;
    m_getUtil = reinterpret_cast<int (*)(void*, void*)>(util);
    return true;
}

unsigned GpuLoad::Percent(uint64_t nowMs, const std::string& cardName) {
    if (!m_tried || (m_ok && cardName != m_cardName && !cardName.empty() && m_cardName != cardName)) { m_ok = Init(cardName); m_lastAt = 0; }
    if (!m_ok) return 0;
    if (m_lastAt && nowMs - m_lastAt < 1000) return m_last;
    m_lastAt = nowMs;
    NvmlUtil u{};
    m_last = m_getUtil(m_dev, &u) == 0 ? (u.gpu > 100 ? 100u : u.gpu) : 0;
    return m_last;
}

}   // namespace nr
