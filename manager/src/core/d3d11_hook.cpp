#include "d3d11_hook.h"
#include "hook_util.h"
#include "../host/host_impl.h"
#include "../event/event_system.h"
#include "../log/logger.h"
#include <MinHook.h>
#include <d3d11_4.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <dxgi.h>
#include <utility>
#include <vector>

namespace D3D11Hook {

namespace {

std::atomic<eam::HostImpl*> g_host{ nullptr };
// The immediate contexts of the devices Lossless Scaling made, newest last. It makes more than one when scaling starts (two within a tenth of a
// second, seen live), and the compute passes run on the first: every one of them is watched, not only the newest.
constexpr int kContexts = 8;
std::atomic<ID3D11DeviceContext*> g_lsContexts[kContexts] = {};
std::atomic<int> g_nextContext{ 0 };
int LosslessIndex(ID3D11DeviceContext* ctx) {
    for (int i = 0; i < kContexts; ++i) if (g_lsContexts[i].load(std::memory_order_acquire) == ctx) return i;
    return -1;
}
// What each of those contexts' passes have been, and the adapter name of its device (named inside a dispatch of its own, when the context is known to be alive).
std::atomic<uint32_t> g_ctxPasses[kContexts] = {};
char g_ctxCard[kContexts][128] = {};
std::atomic<bool> g_ctxCardKnown[kContexts] = {};
void AdapterName(ID3D11Device* dev, char (&text)[128]) {   // a real device
    IDXGIDevice* dxgi = nullptr; IDXGIAdapter* adapter = nullptr;
    text[0] = 0;
    if (dev && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc))) WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, text, sizeof text - 1, nullptr, nullptr);
    }
    if (adapter) adapter->Release();
    if (dxgi) dxgi->Release();
}
char g_newestCard[128] = {};   // the card of the device Lossless Scaling made last (under g_newestMutex)
std::mutex g_newestMutex;
void NameCard(int index, ID3D11DeviceContext* ctx) {
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    char text[128] = {};
    AdapterName(dev, text);
    if (dev) dev->Release();
    memcpy(g_ctxCard[index], text, sizeof text);
    g_ctxCardKnown[index].store(true, std::memory_order_release);
}
std::atomic<uint32_t> g_dispatches{ 0 };
std::atomic<DispatchObserver> g_observer{ nullptr };
thread_local ID3D11DeviceContext* t_dispatching = nullptr;

// ---- the device

using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                        D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
CreateDeviceFn g_realCreateDevice = nullptr;
bool g_importPatched = false;

HRESULT WINAPI OnCreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels,
                              UINT levelCount, UINT sdk, ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context) {
    const HRESULT hr = g_realCreateDevice(adapter, type, software, flags, levels, levelCount, sdk, device, level, context);
    if (SUCCEEDED(hr) && device && *device && context && *context) {
        LOG_INFO("D3D11Hook", "Lossless Scaling made a D3D11 device (%p, context %p, feature level %x)", *device, *context, level ? (unsigned)*level : 0u);
        Attach(*device, *context);
    }
    return hr;
}

// ---- the passes

using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
constexpr int kDispatchSlot = 41;   // ID3D11DeviceContext::Dispatch in the function table
constexpr int kMaxHooks = 8;        // room to spare: the runtime has a handful
struct Hooked { void* target = nullptr; DispatchFn original = nullptr; };
Hooked g_hooks[kMaxHooks];
int g_hooked = 0;

// One per hooked implementation, each calling on to its own original. Only the outermost Dispatch on a thread runs the callbacks: a refresh stub
// jumps on into the real implementation (hooked as well), and an addon may dispatch its own work from a callback.
template <int I> void STDMETHODCALLTYPE Detour(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) {
    const DispatchFn original = g_hooks[I].original;
    const int index = (t_dispatching || !ctx) ? -1 : LosslessIndex(ctx);
    if (index < 0) { original(ctx, x, y, z); return; }
    t_dispatching = ctx;
    g_dispatches.fetch_add(1, std::memory_order_relaxed);
    g_ctxPasses[index].fetch_add(1, std::memory_order_relaxed);
    if (!g_ctxCardKnown[index].load(std::memory_order_acquire)) NameCard(index, ctx);
    if (const DispatchObserver observer = g_observer.load(std::memory_order_acquire)) observer(ctx, x, y, z);
    eam::HostImpl* const host = g_host.load(std::memory_order_acquire);
    const bool skip = host && host->InvokePreDispatch(x, y, z);
    if (!skip) {
        original(ctx, x, y, z);
        if (host) host->InvokePostDispatch(x, y, z);
    }
    t_dispatching = nullptr;
}
template <int... I> void* DetourAt(int i, std::integer_sequence<int, I...>) {
    void* const all[] = { (void*)&Detour<I>... };
    return all[i];
}
void* DetourAt(int i) { return DetourAt(i, std::make_integer_sequence<int, kMaxHooks>{}); }

void Note(std::vector<void*>& found, ID3D11DeviceContext* ctx) {
    void* const fn = (*reinterpret_cast<void***>(ctx))[kDispatchSlot];
    if (fn && std::find(found.begin(), found.end(), fn) == found.end()) found.push_back(fn);
}

// Every state that gives a context another function table: plain, multithread protected (whose table starts as refresh stubs that the first
// state-setting call resolves), and deferred. The tables belong to the runtime, not the driver, so a WARP device shows them all.
std::vector<void*> FindDispatchImplementations() {
    std::vector<void*> found;
    auto noteResolved = [&](ID3D11DeviceContext* ctx) { Note(found, ctx); ctx->CSSetShader(nullptr, nullptr, 0); Note(found, ctx); };
    for (const D3D_DRIVER_TYPE type : { D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_HARDWARE }) {
        for (const UINT flags : { 0u, (UINT)D3D11_CREATE_DEVICE_SINGLETHREADED }) {
            ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
            const HRESULT hr = D3D11CreateDevice(nullptr, type, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
            if (FAILED(hr)) { LOG_WARN("D3D11Hook", "A probe device (type %d, flags %u) failed: 0x%08x", (int)type, flags, (unsigned)hr); continue; }
            noteResolved(ctx);
            ID3D11Multithread* mt = nullptr;
            if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&mt)))) {
                for (const BOOL on : { TRUE, FALSE, TRUE }) { mt->SetMultithreadProtected(on); noteResolved(ctx); }
                mt->Release();
            }
            ID3D11DeviceContext* deferred = nullptr;
            if (SUCCEEDED(dev->CreateDeferredContext(0, &deferred))) { noteResolved(deferred); deferred->Release(); }
            ctx->Release(); dev->Release();
        }
        if (!found.empty()) break;   // WARP was enough: the graphics card is left alone
    }
    return found;
}

} // namespace

void Initialize(eam::HostImpl* host) {
    g_host.store(host, std::memory_order_release);
    if (g_importPatched) return;
    const HMODULE lossless = GetModuleHandleW(L"Lossless_original.dll");
    if (!lossless) { LOG_ERROR("D3D11Hook", "Lossless_original.dll is not loaded"); return; }
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!d3d11) d3d11 = LoadLibraryW(L"d3d11.dll");
    g_realCreateDevice = d3d11 ? reinterpret_cast<CreateDeviceFn>(GetProcAddress(d3d11, "D3D11CreateDevice")) : nullptr;
    if (!g_realCreateDevice) { LOG_ERROR("D3D11Hook", "d3d11.dll's D3D11CreateDevice was not found"); return; }
    // The slot holds the delay-load stub until the first call; ours calls the real function directly, so the stub is never needed.
    void** const slot = eam::hooks::DelayImportSlot(lossless, "d3d11.dll", "D3D11CreateDevice");
    if (!slot || !eam::hooks::SwapSlot(slot, (void*)&OnCreateDevice)) {
        LOG_ERROR("D3D11Hook", "Lossless_original.dll's delay import of D3D11CreateDevice was not found or could not be patched: its device will not be seen");
        return;
    }
    g_importPatched = true;
    LOG_INFO("D3D11Hook", "Watching for Lossless Scaling's D3D11 device");
}

int InstallDispatchHooks() {
    if (g_hooked) return g_hooked;
    const std::vector<void*> targets = FindDispatchImplementations();
    if (targets.empty()) { LOG_ERROR("D3D11Hook", "No Dispatch implementation found: dispatch callbacks will not run"); return 0; }
    if (!eam::hooks::Begin()) { LOG_ERROR("D3D11Hook", "MinHook could not start: dispatch callbacks will not run"); return 0; }
    for (void* target : targets) {
        if (g_hooked == kMaxHooks) break;
        Hooked& h = g_hooks[g_hooked];
        if (MH_CreateHook(target, DetourAt(g_hooked), reinterpret_cast<void**>(&h.original)) != MH_OK) continue;
        if (MH_EnableHook(target) != MH_OK) { MH_RemoveHook(target); continue; }
        h.target = target;
        ++g_hooked;
    }
    if (!g_hooked) { eam::hooks::End(); LOG_ERROR("D3D11Hook", "The Dispatch implementations could not be hooked: dispatch callbacks will not run"); return 0; }
    LOG_INFO("D3D11Hook", "Hooked %d of %zu Dispatch implementations", g_hooked, targets.size());
    return g_hooked;
}

void SetDispatchObserver(DispatchObserver observer) { g_observer.store(observer, std::memory_order_release); }

void Shutdown() {
    g_observer.store(nullptr, std::memory_order_release);
    g_host.store(nullptr, std::memory_order_release);
    for (auto& c : g_lsContexts) c.store(nullptr, std::memory_order_release);
    g_dispatches.store(0, std::memory_order_relaxed);
}

void Attach(ID3D11Device* device, ID3D11DeviceContext* context) {
    eam::HostImpl* const host = g_host.load(std::memory_order_acquire);
    if (!host) return;
    void* const before = host->GetD3D11Device();
    host->SetD3D11Device(device, context);
    { char name[128]; AdapterName(device, name); std::lock_guard<std::mutex> lock(g_newestMutex); memcpy(g_newestCard, name, sizeof name); }
    if (LosslessIndex(context) < 0) {
        const int slot = g_nextContext.fetch_add(1) % kContexts;
        g_ctxCardKnown[slot].store(false, std::memory_order_release);   // a new context in the slot: its card is named at its first pass
        g_ctxPasses[slot].store(0, std::memory_order_relaxed);
        g_lsContexts[slot].store(context, std::memory_order_release);
    }
    if (!before) g_dispatches.store(0, std::memory_order_relaxed);
    if (before && before != static_cast<void*>(device)) eam::EventBus::Instance().Publish(EAM_EVENT_D3D11_DEVICE_CHANGED);
    eam::EventBus::Instance().Publish(EAM_EVENT_D3D11_DEVICE_READY);
}

void* GetCurrentComputeShader() {
    ID3D11DeviceContext* const ctx = t_dispatching;
    if (!ctx) return nullptr;
    ID3D11ComputeShader* shader = nullptr;
    ctx->CSGetShader(&shader, nullptr, nullptr);
    if (shader) shader->Release();   // borrowed: the context still holds it
    return shader;
}

uint32_t GetDispatchCount() { return g_dispatches.load(std::memory_order_relaxed); }

std::string ActiveCardName() {
    int best = -1; uint32_t most = 0;
    for (int i = 0; i < kContexts; ++i) {
        const uint32_t n = g_ctxPasses[i].exchange(0, std::memory_order_relaxed);   // (taken: the next call sees only what came after)
        if (n > most && g_ctxCardKnown[i].load(std::memory_order_acquire)) { most = n; best = i; }
    }
    static std::mutex mutex; static std::string last;   // the card of the last busiest context: kept while no pass has come since (several callers share the counts)
    std::lock_guard<std::mutex> lock(mutex);
    if (best >= 0) last = g_ctxCard[best];
    if (last.empty()) { std::lock_guard<std::mutex> newest(g_newestMutex); last = g_newestCard; }   // before any pass: the card of the newest device
    return last;
}

ID3D11DeviceContext* DispatchingContext() { return t_dispatching; }

} // namespace D3D11Hook
