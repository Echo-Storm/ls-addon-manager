#include "gpu_stats.h"
#include "metrics.h"
#include <windows.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <vector>

namespace eam {

namespace {
std::atomic<bool> g_preferCounters{ false };

// ---- Windows' counters (any vendor): loaded by name, so nothing has to link against pdh.lib
struct Pdh {
    HMODULE lib = nullptr;
    PDH_STATUS (WINAPI *Open)(LPCWSTR, DWORD_PTR, PDH_HQUERY*) = nullptr;
    PDH_STATUS (WINAPI *Add)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*) = nullptr;
    PDH_STATUS (WINAPI *Collect)(PDH_HQUERY) = nullptr;
    PDH_STATUS (WINAPI *GetArray)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W) = nullptr;
    PDH_STATUS (WINAPI *Close)(PDH_HQUERY) = nullptr;
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER engine = nullptr, memory = nullptr;
    std::wstring luidKey;   // "luid_0x00000000_0x0000DE57": how the counters name the card
    std::string name;
    unsigned long long vramTotalBytes = 0;
    int cards = 0;
} g_pdh;

// One counter's instances and values, as (instance name, value).
bool ReadArray(PDH_HCOUNTER counter, std::vector<std::pair<std::wstring, double>>& out) {
    out.clear();
    DWORD bytes = 0, count = 0;
    PDH_STATUS st = g_pdh.GetArray(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
    if (st != PDH_MORE_DATA || bytes == 0) return false;
    std::vector<char> buf(bytes + 64);
    auto* items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data());
    st = g_pdh.GetArray(counter, PDH_FMT_DOUBLE, &bytes, &count, items);
    if (st != ERROR_SUCCESS) return false;
    for (DWORD i = 0; i < count; ++i)
        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
            out.emplace_back(items[i].szName, items[i].FmtValue.doubleValue);
    return true;
}
bool Contains(const std::wstring& s, const std::wstring& what) {
    return std::search(s.begin(), s.end(), what.begin(), what.end(), [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); }) != s.end();
}
using nvmlReturn = int;   // 0 = success
struct nvmlUtil { unsigned gpu, memory; };
struct nvmlMem { unsigned long long total, free, used; };

struct Api {
    nvmlReturn (*Init)() = nullptr;
    nvmlReturn (*Shutdown)() = nullptr;
    nvmlReturn (*GetCount)(unsigned*) = nullptr;
    nvmlReturn (*GetHandle)(unsigned, void**) = nullptr;
    nvmlReturn (*GetName)(void*, char*, unsigned) = nullptr;
    nvmlReturn (*GetUtil)(void*, nvmlUtil*) = nullptr;
    nvmlReturn (*GetPower)(void*, unsigned*) = nullptr;
    nvmlReturn (*GetPowerLimit)(void*, unsigned*) = nullptr;
    nvmlReturn (*GetClock)(void*, int, unsigned*) = nullptr;
    nvmlReturn (*GetTemp)(void*, int, unsigned*) = nullptr;
    nvmlReturn (*GetMem)(void*, nvmlMem*) = nullptr;
    nvmlReturn (*GetThrottle)(void*, unsigned long long*) = nullptr;
    nvmlReturn (*GetDriver)(char*, unsigned) = nullptr;
};
Api g_api;

int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}

GpuStats& GpuStats::Instance() { static GpuStats g; return g; }

void GpuStats::PreferCounters(bool on) { g_preferCounters = on; }

// The card with the most memory (the discrete one, where there are two), by DXGI; then two counters for it: the busiest engine's load and
// the memory in use, which the Task Manager shows too.
bool GpuStats::InitCounters(std::string* why) {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto create = dxgi ? reinterpret_cast<HRESULT (WINAPI*)(REFIID, void**)>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) { *why = "DXGI could not list the graphics cards"; return false; }
    DXGI_ADAPTER_DESC1 best{}; bool have = false; int cards = 0;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d); a->Release();
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        ++cards;
        if (!have || d.DedicatedVideoMemory > best.DedicatedVideoMemory) { best = d; have = true; }
    }
    factory->Release();
    if (!have) { *why = "no graphics card was listed"; return false; }
    wchar_t key[64]; swprintf(key, 64, L"luid_0x%08X_0x%08X", static_cast<unsigned>(best.AdapterLuid.HighPart), best.AdapterLuid.LowPart);
    g_pdh.luidKey = key;
    char narrow[256] = {}; WideCharToMultiByte(CP_UTF8, 0, best.Description, -1, narrow, sizeof narrow - 1, nullptr, nullptr);
    g_pdh.name = narrow; g_pdh.vramTotalBytes = best.DedicatedVideoMemory; g_pdh.cards = cards;

    HMODULE pdh = LoadLibraryW(L"pdh.dll");
    if (!pdh) { *why = "pdh.dll (Windows' performance counters) is missing"; return false; }
    g_pdh.lib = pdh;
    g_pdh.Open = reinterpret_cast<decltype(g_pdh.Open)>(GetProcAddress(pdh, "PdhOpenQueryW"));
    g_pdh.Add = reinterpret_cast<decltype(g_pdh.Add)>(GetProcAddress(pdh, "PdhAddEnglishCounterW"));
    g_pdh.Collect = reinterpret_cast<decltype(g_pdh.Collect)>(GetProcAddress(pdh, "PdhCollectQueryData"));
    g_pdh.GetArray = reinterpret_cast<decltype(g_pdh.GetArray)>(GetProcAddress(pdh, "PdhGetFormattedCounterArrayW"));
    g_pdh.Close = reinterpret_cast<decltype(g_pdh.Close)>(GetProcAddress(pdh, "PdhCloseQuery"));
    if (!g_pdh.Open || !g_pdh.Add || !g_pdh.Collect || !g_pdh.GetArray) { *why = "pdh.dll is missing functions"; return false; }
    if (g_pdh.Open(nullptr, 0, &g_pdh.query) != ERROR_SUCCESS) { *why = "the performance counters could not start"; return false; }
    if (g_pdh.Add(g_pdh.query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_pdh.engine) != ERROR_SUCCESS) {
        *why = "this Windows has no GPU counters (they need Windows 10 1709 or later and a WDDM 2 driver)";
        return false;
    }
    g_pdh.Add(g_pdh.query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &g_pdh.memory);   // (the memory is optional)
    g_pdh.Collect(g_pdh.query);   // the first collection is only the baseline of the rate counters
    Sleep(120);
    return true;
}

bool GpuStats::SampleCounters(Snapshot& s) {
    if (g_pdh.Collect(g_pdh.query) != ERROR_SUCCESS) return false;
    std::vector<std::pair<std::wstring, double>> v;
    // load: per engine ("phys_0_eng_3_engtype_3D") the sum over the processes using it, then the busiest engine: what the Task Manager shows
    double busiest = 0;
    if (ReadArray(g_pdh.engine, v)) {
        std::map<std::wstring, double> perEngine;
        for (const auto& [name, value] : v) {
            if (!Contains(name, g_pdh.luidKey)) continue;
            const size_t at = name.find(L"phys_");
            perEngine[at == std::wstring::npos ? name : name.substr(at)] += value;
        }
        for (const auto& [engine, value] : perEngine) busiest = (std::max)(busiest, value);
    }
    s.utilGpu = static_cast<unsigned>((std::min)(100.0, busiest) + 0.5);
    if (g_pdh.memory && ReadArray(g_pdh.memory, v)) {
        double used = 0;
        for (const auto& [name, value] : v) if (Contains(name, g_pdh.luidKey)) used += value;
        s.vramUsedMB = static_cast<uint64_t>(used / (1024.0 * 1024.0));
    }
    s.vramTotalMB = g_pdh.vramTotalBytes >> 20;
    return true;
}

bool GpuStats::Init() {
    std::lock_guard<std::mutex> initLock(m_initMutex);   // (the sampler thread and the diagnostics button can both be the first to ask)
    if (m_inited) return m_dev != nullptr || m_counters;
    m_inited = true;
    std::string nvmlWhy;
    HMODULE lib = g_preferCounters ? nullptr : LoadLibraryW(L"nvml.dll");
    if (!lib && !g_preferCounters) lib = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    if (!lib) nvmlWhy = "nvml.dll was not found (it comes with the NVIDIA driver)";
    if (lib) {
        m_lib = lib;
        if (InitNvml(lib, nvmlWhy)) return true;
    }
    // no NVIDIA library, or no NVIDIA card answered: Windows' counters (AMD, Intel, or any other card)
    std::string why;
    if (InitCounters(&why)) {
        m_counters = true;
        std::lock_guard<std::mutex> lk(m_mutex);
        m_snap.name = g_pdh.name; m_snap.deviceCount = g_pdh.cards; m_snap.viaCounters = true;
        return true;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    m_snap.why = nvmlWhy.empty() ? why : nvmlWhy + "; " + why;
    return false;
}

bool GpuStats::InitNvml(void* libHandle, std::string& why) {
    HMODULE lib = static_cast<HMODULE>(libHandle);
#define BIND(field, name) g_api.field = (decltype(g_api.field))GetProcAddress(lib, name)
    BIND(Init, "nvmlInit_v2"); BIND(Shutdown, "nvmlShutdown"); BIND(GetCount, "nvmlDeviceGetCount_v2"); BIND(GetHandle, "nvmlDeviceGetHandleByIndex_v2");
    BIND(GetName, "nvmlDeviceGetName"); BIND(GetUtil, "nvmlDeviceGetUtilizationRates"); BIND(GetPower, "nvmlDeviceGetPowerUsage");
    BIND(GetPowerLimit, "nvmlDeviceGetEnforcedPowerLimit"); BIND(GetClock, "nvmlDeviceGetClockInfo"); BIND(GetTemp, "nvmlDeviceGetTemperature");
    BIND(GetMem, "nvmlDeviceGetMemoryInfo"); BIND(GetThrottle, "nvmlDeviceGetCurrentClocksThrottleReasons"); BIND(GetDriver, "nvmlSystemGetDriverVersion");
#undef BIND
    if (!g_api.Init || !g_api.GetCount || !g_api.GetHandle) { why = "nvml.dll is missing functions"; return false; }
    if (g_api.Init() != 0) { why = "NVML could not start"; return false; }
    unsigned n = 0;
    if (g_api.GetCount(&n) != 0 || n == 0) { why = "no NVIDIA GPU reported"; return false; }
    void* dev = nullptr;
    if (g_api.GetHandle(0, &dev) != 0) { why = "could not open the GPU"; return false; }
    m_dev = dev;
    char name[128] = {}, drv[64] = {};
    if (g_api.GetName) g_api.GetName(dev, name, sizeof name);
    if (g_api.GetDriver) g_api.GetDriver(drv, sizeof drv);
    std::lock_guard<std::mutex> lk(m_mutex);
    m_snap.name = name; m_snap.driver = drv; m_snap.deviceCount = (int)n;
    return true;
}

bool GpuStats::SampleOnce() {
    if (!Init()) return false;
    std::lock_guard<std::mutex> sampleLock(m_sampleMutex);   // (one query, read by one thread at a time)
    Snapshot s;
    { std::lock_guard<std::mutex> lk(m_mutex); s = m_snap; }
    s.ok = true; s.why.clear();
    if (m_counters) {
        if (!SampleCounters(s)) return false;
    } else {
        nvmlUtil u{}; if (g_api.GetUtil && g_api.GetUtil(m_dev, &u) == 0) { s.utilGpu = u.gpu; s.utilMem = u.memory; }
        unsigned v = 0;
        if (g_api.GetPower && g_api.GetPower(m_dev, &v) == 0) { s.powerW = v / 1000.0; s.hasPower = true; }
        if (g_api.GetPowerLimit && g_api.GetPowerLimit(m_dev, &v) == 0) s.powerLimitW = v / 1000.0;
        if (g_api.GetClock && g_api.GetClock(m_dev, 0, &v) == 0) { s.clockGraphics = v; s.hasClocks = true; }   // NVML_CLOCK_GRAPHICS
        if (g_api.GetClock && g_api.GetClock(m_dev, 2, &v) == 0) s.clockMem = v;                                // NVML_CLOCK_MEM
        if (g_api.GetTemp && g_api.GetTemp(m_dev, 0, &v) == 0) { s.tempC = v; s.hasTemp = true; }              // NVML_TEMPERATURE_GPU
        nvmlMem m{}; if (g_api.GetMem && g_api.GetMem(m_dev, &m) == 0) { s.vramUsedMB = m.used >> 20; s.vramTotalMB = m.total >> 20; }
        unsigned long long th = 0; if (g_api.GetThrottle && g_api.GetThrottle(m_dev, &th) == 0) { s.throttle = th; s.hasThrottle = true; }
    }
    s.ageSeconds = 0;
    { std::lock_guard<std::mutex> lk(m_mutex); m_snap = s; m_takenAtMs = NowMs(); }
    Metrics& M = Metrics::Instance();
    M.Publish("system", "gpu_util", s.utilGpu, "%");
    if (s.hasPower) M.Publish("system", "gpu_power_w", s.powerW, "W");
    if (s.hasClocks) M.Publish("system", "gpu_clock_mhz", s.clockGraphics, "MHz");
    if (s.hasTemp) M.Publish("system", "gpu_temp_c", s.tempC, "C");
    M.Publish("system", "vram_used_mb", (double)s.vramUsedMB, "MB");
    return true;
}

void GpuStats::InjectForPreview(const Snapshot& s) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_snap = s; m_snap.ok = true; m_takenAtMs = NowMs();
    m_started = true;   // a preview's numbers stay: the sampler is never started over them
}

void GpuStats::Run() {
    while (!m_stop) {
        const int64_t now = NowMs();
        if (now - m_wantedAtMs.load() < 4000) SampleOnce();   // only while the tab is being looked at
        for (int i = 0; i < 5 && !m_stop; ++i) Sleep(100);
    }
}

void GpuStats::Wanted() {
    m_wantedAtMs = NowMs();
    if (!m_started.exchange(true)) m_thread = std::thread([this] { Run(); });
}

GpuStats::Snapshot GpuStats::Get() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    Snapshot s = m_snap;
    if (s.ok) s.ageSeconds = (NowMs() - m_takenAtMs) / 1000.0;
    return s;
}

void GpuStats::Shutdown() {
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
    if (m_inited && m_dev && g_api.Shutdown) g_api.Shutdown();   // the library stays loaded (the process is ending)
    if (m_counters && g_pdh.query && g_pdh.Close) { g_pdh.Close(g_pdh.query); g_pdh.query = nullptr; }
    m_dev = nullptr; m_counters = false;
}

std::string GpuStats::ThrottleText(uint64_t m) {
    std::string out;
    auto add = [&](uint64_t bit, const char* text) { if (m & bit) { if (!out.empty()) out += ", "; out += text; } };
    add(0x4, "power limit");            // SW power cap
    add(0x80, "power brake");           // HW power brake
    add(0x20, "temperature");           // SW thermal slowdown
    add(0x40, "temperature (hardware)");
    add(0x8, "hardware slowdown");
    add(0x10, "sync boost");
    add(0x2, "application clocks");
    return out;                         // 0x1 (idle) and 0x100 (display clocks) are not worth showing
}

} // namespace eam
