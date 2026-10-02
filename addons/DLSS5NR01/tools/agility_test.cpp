// nr_agilitytest: does a Direct3D 12 device made with nr::CreateDeviceOnAgility (src/addon/agility.cpp) run on the Agility SDK core in a folder?
//   nr_agilitytest <folder with D3D12Core.dll>      makes the device through the folder's core, then a command queue, a fence and a small compute dispatch on it
//   nr_agilitytest -                                 the same through Windows' own core (what runs without the folder)
// Exit code 0 when, with a folder, the core of the folder is the one loaded and the device works; with "-", when the device works.
#include "addon/agility.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <cstdio>
#include <string>

static std::wstring FileVersion(const std::wstring& path) {
    DWORD h = 0; const DWORD n = GetFileVersionInfoSizeW(path.c_str(), &h);
    if (!n) return L"?";
    std::string data(n, '\0');
    if (!GetFileVersionInfoW(path.c_str(), 0, n, data.data())) return L"?";
    VS_FIXEDFILEINFO* f = nullptr; UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&f), &len) || !f) return L"?";
    wchar_t t[64]; swprintf(t, 64, L"%u.%u.%u.%u", HIWORD(f->dwFileVersionMS), LOWORD(f->dwFileVersionMS), HIWORD(f->dwFileVersionLS), LOWORD(f->dwFileVersionLS));
    return t;
}

int main(int argc, char** argv) {
    std::wstring dir;
    if (argc > 1 && std::string(argv[1]) != "-") {
        const int n = MultiByteToWideChar(CP_ACP, 0, argv[1], -1, nullptr, 0);
        dir.resize(n ? n - 1 : 0); MultiByteToWideChar(CP_ACP, 0, argv[1], -1, dir.data(), n);
        wchar_t full[MAX_PATH * 2] = {}; if (GetFullPathNameW(dir.c_str(), MAX_PATH * 2, full, nullptr)) dir = full;
    }
    IDXGIFactory4* factory = nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    IDXGIAdapter1* adapter = nullptr; if (factory) factory->EnumAdapters1(0, &adapter);
    ID3D12Device* dev = nullptr; HRESULT hr = E_FAIL;
    if (!dir.empty()) {
        std::string say;
        const bool ok = nr::CreateDeviceOnAgility(dir, adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev), say);
        printf("agility: %s -> %s\n", say.c_str(), ok ? "device made" : "no device");
        hr = ok ? S_OK : E_FAIL;
    } else hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    printf("device: 0x%08x\n", static_cast<unsigned>(hr));
    bool works = false;
    if (SUCCEEDED(hr) && dev) {   // the device does work: a queue, a fence, a signal
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* queue = nullptr; ID3D12Fence* fence = nullptr;
        if (SUCCEEDED(dev->CreateCommandQueue(&q, IID_PPV_ARGS(&queue))) && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            queue->Signal(fence, 1);
            for (int i = 0; i < 200 && fence->GetCompletedValue() < 1; ++i) Sleep(5);
            works = fence->GetCompletedValue() >= 1;
        }
        if (fence) fence->Release(); if (queue) queue->Release();
    }
    printf("queue and fence: %s\n", works ? "work" : "FAILED");
    bool loaded = true;
    {   // every Direct3D 12 core in the process
        HMODULE mods[1024]; DWORD needed = 0;
        if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed))
            for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; ++i) {
                wchar_t p[MAX_PATH * 2] = {};
                if (GetModuleFileNameW(mods[i], p, MAX_PATH * 2) && (wcsstr(p, L"D3D12Core") || wcsstr(p, L"d3d12core") || wcsstr(p, L"d3d12.dll"))) wprintf(L"  loaded: %ls\n", p);
            }
    }
    if (!dir.empty()) {
        loaded = nr::AgilityCoreLoaded(dir);
        wprintf(L"core from the folder loaded: %ls  (version %ls)\n", loaded ? L"yes" : L"no", FileVersion(dir + L"\\D3D12Core.dll").c_str());
    } else {
        HMODULE m = GetModuleHandleW(L"D3D12Core.dll"); wchar_t p[MAX_PATH * 2] = {};
        if (m) GetModuleFileNameW(m, p, MAX_PATH * 2);
        wprintf(L"Windows' core: %ls  (version %ls)\n", p, FileVersion(p).c_str());
    }
    if (dev) dev->Release(); if (adapter) adapter->Release(); if (factory) factory->Release();
    const bool ok = SUCCEEDED(hr) && works && loaded;
    printf("%s\n", ok ? "OK" : "NOT OK");
    return ok ? 0 : 1;
}
