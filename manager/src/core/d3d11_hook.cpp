#include "d3d11_hook.h"
#include "hook_util.h"
#include "../host/host_impl.h"
#include "../event/event_system.h"
#include "../log/logger.h"
#include <MinHook.h>
#include <d3d11_4.h>
#include <algorithm>
#include <atomic>
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
bool IsLossless(ID3D11DeviceContext* ctx) {
    for (const auto& c : g_lsContexts) if (c.load(std::memory_order_acquire) == ctx) return true;
    return false;
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
    if (t_dispatching || !ctx || !IsLossless(ctx)) { original(ctx, x, y, z); return; }
    t_dispatching = ctx;
    g_dispatches.fetch_add(1, std::memory_order_relaxed);
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
    if (!IsLossless(context)) g_lsContexts[g_nextContext.fetch_add(1) % kContexts].store(context, std::memory_order_release);
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

ID3D11DeviceContext* DispatchingContext() { return t_dispatching; }

} // namespace D3D11Hook
