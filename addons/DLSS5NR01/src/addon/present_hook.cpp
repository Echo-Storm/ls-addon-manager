#include "addon/present_hook.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>

// Why patch the function table and not the code: something else in the process (the NVIDIA overlay, which hooks Present at the first present)
// hooks dxgi's Present after us and calls on to an untouched copy of the code, which quietly bypasses a code hook placed before it (seen in the
// offline host: our jump replaced, the hit count stuck at 1). The swap chain's function table is a static table in dxgi.dll, shared by every swap
// chain of that kind and never refreshed per object, so a patched slot keeps working and still reaches whatever code hook sits on the function.
namespace {
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
constexpr int kPresentSlot = 8, kPresent1Slot = 22;

// Once patched, the table keeps our functions for the life of the process, and this DLL is pinned so they stay in memory: something that
// hooked after us (the other addon of the pair, an overlay) calls on through them to the real Present, even after the manager unloads this
// addon. Uninstall only clears the callback, which makes them pass straight through, and a second Install (the addon started again) only
// sets it again: patching a second time would make a loop of the hook chain.
void** g_patched = nullptr;
PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;
void** g_table = nullptr;
std::atomic<PresentHook::Callback> g_callback{ nullptr };
std::atomic<unsigned> g_hits{ 0 };
thread_local int t_nesting = 0;

// Only the outermost present on a thread runs the callback, so a present made inside it (or by a hook we call on to) does not run it again.
void Before(IDXGISwapChain* sc, UINT sync, UINT flags) {
    g_hits.fetch_add(1, std::memory_order_relaxed);
    if (t_nesting == 1 && !(flags & DXGI_PRESENT_TEST))
        if (const PresentHook::Callback cb = g_callback.load(std::memory_order_acquire)) cb(sc, sync, flags);
}
HRESULT STDMETHODCALLTYPE OnPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    ++t_nesting; Before(sc, sync, flags);
    const HRESULT hr = g_present ? g_present(sc, sync, flags) : S_OK; --t_nesting;
    return hr;
}
HRESULT STDMETHODCALLTYPE OnPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    ++t_nesting; Before(sc, sync, flags);
    const HRESULT hr = g_present1 ? g_present1(sc, sync, flags, params) : S_OK; --t_nesting;
    return hr;
}

// Writes fn into the table's slot and returns what was there (null if the page could not be made writable).
void* Swap(void** table, int slot, void* fn) {
    DWORD protect = 0;
    if (!VirtualProtect(table + slot, sizeof(void*), PAGE_READWRITE, &protect)) return nullptr;
    void* const was = table[slot];
    table[slot] = fn;
    VirtualProtect(table + slot, sizeof(void*), protect, &protect);
    return was;
}

// A short-lived swap chain on the device, to read the shared table from: a window swap chain like Lossless Scaling's, or a composition one if
// no window can be made.
void** FindSwapChainTable(ID3D11Device* dev, const PresentHook::LogFn& log) {
    IDXGIDevice* dxgi = nullptr; IDXGIAdapter* adapter = nullptr; IDXGIFactory2* factory = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgi)))) { log("PresentHook: the device has no IDXGIDevice"); return nullptr; }
    HRESULT hr = dxgi->GetAdapter(&adapter); dxgi->Release();
    if (FAILED(hr)) { log("PresentHook: the device has no adapter"); return nullptr; }
    hr = adapter->GetParent(IID_PPV_ARGS(&factory)); adapter->Release();
    if (FAILED(hr)) { log("PresentHook: no IDXGIFactory2"); return nullptr; }

    WNDCLASSEXW wc{ sizeof wc };
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"NrPresentProbe";
    RegisterClassExW(&wc);   // fails harmlessly when a previous install registered it
    const HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 16; desc.Height = 16; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; desc.Scaling = DXGI_SCALING_NONE; desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    IDXGISwapChain1* sc = nullptr;
    hr = hwnd ? factory->CreateSwapChainForHwnd(dev, hwnd, &desc, nullptr, nullptr, &sc) : E_FAIL;
    if (FAILED(hr)) {
        desc.Scaling = DXGI_SCALING_STRETCH; desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        hr = factory->CreateSwapChainForComposition(dev, &desc, nullptr, &sc);
    }
    factory->Release();
    void** table = nullptr;
    if (SUCCEEDED(hr)) { table = *(void***)sc; sc->Release(); }
    else { char text[96]; snprintf(text, sizeof text, "PresentHook: the probe swap chain failed: 0x%08x", (unsigned)hr); log(text); }
    if (hwnd) DestroyWindow(hwnd);
    return table;
}
} // namespace

bool PresentHook::Install(ID3D11Device* dev, Callback cb, LogFn log) {
    if (g_table) return true;
    if (g_patched) { g_callback.store(cb, std::memory_order_release); g_table = g_patched; log("PresentHook: our functions are still in the table: in use again"); return true; }
    void** const table = FindSwapChainTable(dev, log);
    if (!table) return false;
    g_callback.store(cb, std::memory_order_release);
    void* const present = Swap(table, kPresentSlot, (void*)&OnPresent);
    if (!present) { log("PresentHook: the swap chain's function table could not be made writable"); g_callback.store(nullptr); return false; }
    g_present = (PresentFn)present;
    g_present1 = (Present1Fn)Swap(table, kPresent1Slot, (void*)&OnPresent1);   // null: Present1 is simply not seen
    g_table = g_patched = table;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&OnPresent), &self);
    const uintptr_t base = (uintptr_t)GetModuleHandleW(L"dxgi.dll");
    char text[200];
    snprintf(text, sizeof text, "PresentHook: patched the swap chain table %p: Present (dxgi+0x%llx) and Present1 (dxgi+0x%llx)", (void*)table,
             (unsigned long long)((uintptr_t)g_present - base), (unsigned long long)(g_present1 ? (uintptr_t)g_present1 - base : 0));
    log(text);
    return true;
}

void PresentHook::Uninstall() {
    g_callback.store(nullptr, std::memory_order_release);   // our functions stay in the table and pass straight through (see above)
    g_table = nullptr;
}

bool PresentHook::Installed() { return g_table != nullptr; }
HRESULT PresentHook::PresentOriginal(IDXGISwapChain* sc, UINT sync, UINT flags) { return g_present ? g_present(sc, sync, flags) : E_FAIL; }
unsigned PresentHook::Hits() { return g_hits.load(std::memory_order_relaxed); }

void PresentHook::DumpState(LogFn log) {
    if (!g_table) { log("PresentHook: not installed"); return; }
    char text[200];
    snprintf(text, sizeof text, "PresentHook: %u presents; table %p: Present slot %s, Present1 slot %s", Hits(), (void*)g_table,
             g_table[kPresentSlot] == (void*)&OnPresent ? "ours" : "taken by another hook",
             g_table[kPresent1Slot] == (void*)&OnPresent1 ? "ours" : "not ours");
    log(text);
}
