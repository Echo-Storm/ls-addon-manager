// How much video memory this process holds on a card, by DXGI's own per-process figure (IDXGIAdapter3::QueryVideoMemoryInfo): inside Lossless Scaling it is Lossless Scaling's and
// every addon's together, so the addons log it at their engines' start (before and after) and now and then, and what they hold is the difference. In MB; 0 when it cannot be asked.
#pragma once
#include <windows.h>
#include <dxgi1_4.h>
#include <cstdint>

namespace nr {

inline uint64_t ProcessVideoMemoryMb(LUID luid, uint64_t* budgetMb = nullptr) {
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) return 0;
    uint64_t used = 0;
    IDXGIAdapter3* adapter = nullptr;
    if (SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) && adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) { used = info.CurrentUsage >> 20; if (budgetMb) *budgetMb = info.Budget >> 20; }
        adapter->Release();
    }
    factory->Release();
    return used;
}

} // namespace nr
