#include <windows.h>
#include <psapi.h>
#include <initguid.h>   // (CLSID_D3D12SDKConfiguration is defined where this is included: before any include of d3d12.h, so before agility.h)
#include "addon/agility.h"
#include <d3d12.h>
#include <cstdio>

namespace nr {

namespace {
// The folder as an 8-bit string for the loader; a path with letters outside the ANSI code page would be damaged, so the short (8.3) form is used when it is plain ASCII.
bool AsciiPath(const std::wstring& dir, std::string& out) {
    std::wstring use = dir;
    auto plain = [](const std::wstring& s) { for (wchar_t c : s) if (c > 126 || c < 32) return false; return true; };
    if (!plain(use)) {
        wchar_t shortPath[MAX_PATH * 2] = {};
        const DWORD n = GetShortPathNameW(dir.c_str(), shortPath, static_cast<DWORD>(sizeof shortPath / sizeof shortPath[0]));
        if (n == 0 || n >= sizeof shortPath / sizeof shortPath[0] || !plain(shortPath)) return false;
        use = shortPath;
    }
    out.assign(use.begin(), use.end());
    return true;
}
}

bool CreateDeviceOnAgility(const std::wstring& dir, IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** device, std::string& say) {
    const std::wstring core = dir + L"\\D3D12Core.dll";
    if (GetFileAttributesW(core.c_str()) == INVALID_FILE_ATTRIBUTES) { say = "no D3D12Core.dll beside the runtime"; return false; }
    std::string path;
    if (!AsciiPath(dir, path)) { say = "the runtime's folder has letters the Direct3D 12 loader cannot take and no plain short name"; return false; }
    ID3D12SDKConfiguration* config = nullptr;
    HRESULT hr = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&config));
    if (FAILED(hr) || !config) { char t[160]; snprintf(t, sizeof t, "this Windows cannot load another Direct3D 12 core (0x%08x)", static_cast<unsigned>(hr)); say = t; return false; }
    ID3D12SDKConfiguration1* config1 = nullptr;
    hr = config->QueryInterface(IID_PPV_ARGS(&config1));
    config->Release();
    if (FAILED(hr) || !config1) { char t[160]; snprintf(t, sizeof t, "this Windows has no device factory for another Direct3D 12 core (0x%08x)", static_cast<unsigned>(hr)); say = t; return false; }
    ID3D12DeviceFactory* factory = nullptr;
    hr = config1->CreateDeviceFactory(kAgilitySdkVersion, path.c_str(), IID_PPV_ARGS(&factory));
    config1->Release();
    char t[400];
    if (FAILED(hr) || !factory) { snprintf(t, sizeof t, "Windows would not load the core %u from %s (0x%08x)", kAgilitySdkVersion, path.c_str(), static_cast<unsigned>(hr)); say = t; return false; }
    hr = factory->CreateDevice(adapter, level, riid, device);
    factory->Release();
    if (FAILED(hr)) { snprintf(t, sizeof t, "the device on the core %u from %s failed (0x%08x)", kAgilitySdkVersion, path.c_str(), static_cast<unsigned>(hr)); say = t; return false; }
    snprintf(t, sizeof t, "the device is on the Agility SDK core %u from %s", kAgilitySdkVersion, path.c_str());
    say = t;
    return true;
}

bool AgilityCoreLoaded(const std::wstring& dir) {
    wchar_t want[MAX_PATH * 2] = {};
    const std::wstring file = dir + L"\\D3D12Core.dll";
    if (!GetLongPathNameW(file.c_str(), want, MAX_PATH * 2)) return false;
    HMODULE modules[1024]; DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof modules, &needed)) return false;
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; ++i) {
        wchar_t path[MAX_PATH * 2] = {}, longPath[MAX_PATH * 2] = {};
        if (!GetModuleFileNameW(modules[i], path, MAX_PATH * 2)) continue;
        if (GetLongPathNameW(path, longPath, MAX_PATH * 2) && _wcsicmp(longPath, want) == 0) return true;
    }
    return false;
}

}   // namespace nr
