#include "host_impl.h"
#include "metrics.h"
#include "../config/config_manager.h"
#include "../core/d3d11_hook.h"
#include "../gui/icon_loader.h"
#include "../event/event_system.h"
#include "../log/logger.h"
#include "../../sdk/include/eam/version.h"
#include <algorithm>
#include <intrin.h>
#include <windows.h>

namespace eam {

namespace {

// Addons may pass null for any text; the registries want real strings.
const char* Text(const char* s) { return s ? s : ""; }

// The module (addon DLL) that code at `address` belongs to.
void* ModuleAt(void* address) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module);
    return module;
}

// Sets, replaces or removes the hook of one owner: the calling module and its userData. Two addons that both pass no userData keep a callback
// each; one addon passing different userData values keeps one per value.
template <class Hook, class Callback>
void SetHook(std::vector<Hook>& hooks, Callback callback, void* owner, void* module) {
    const auto mine = std::find_if(hooks.begin(), hooks.end(), [&](const Hook& h) { return h.owner == owner && h.module == module; });
    if (!callback) {
        if (mine != hooks.end()) hooks.erase(mine);
    } else if (mine != hooks.end()) {
        mine->callback = callback;
    } else {
        hooks.push_back({ callback, owner, module });
    }
}

// Dispatch callbacks run on Lossless Scaling's render thread, where a fault would end the process. Kept apart from anything that owns
// C++ objects: a function with a __try block cannot also have destructors to run.
// True on a thread while it runs addon dispatch callbacks, which the host calls with its hook list locked: an addon that asks to change its hooks from inside one would wait for
// a lock its own thread holds. The change is refused (and said) instead; an addon sets or clears its callbacks from AddonInitialize / AddonShutdown or a thread of its own.
thread_local bool t_inDispatchCallback = false;
struct InCallback { InCallback() { t_inDispatchCallback = true; } ~InCallback() { t_inDispatchCallback = false; } };

bool CallPre(EamPreDispatchCallback cb, uint32_t x, uint32_t y, uint32_t z, void* owner, bool* skip) {
    __try { *skip = cb(x, y, z, owner); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallPost(EamPostDispatchCallback cb, uint32_t x, uint32_t y, uint32_t z, void* owner) {
    __try { cb(x, y, z, owner); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Removes the callbacks that faulted (marked with a null callback) and says so once for each.
template <class Hook>
void DropFaulted(std::vector<Hook>& hooks, const char* kind) {
    for (const Hook& h : hooks)
        if (!h.callback) LOG_ERROR("Host", "An addon's %s-dispatch callback faulted and was removed (owner %p); the addon may need restarting", kind, h.owner);
    hooks.erase(std::remove_if(hooks.begin(), hooks.end(), [](const Hook& h) { return !h.callback; }), hooks.end());
}

} // namespace

void HostImpl::Log(EamLogLevel level, const char* message) {
    Logger::Instance().Log(static_cast<LogLevel>(level), "Addon", "%s", Text(message));
}

// ---- settings

const char* HostImpl::GetConfig(const char* addonId, const char* key, const char* defaultVal) {
    std::string value = ConfigManager::Instance().Get(Text(addonId), Text(key), Text(defaultVal));
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    m_returnBuffers.push_back(std::move(value));
    if (m_returnBuffers.size() > kReturnBufferCount) m_returnBuffers.pop_front();
    return m_returnBuffers.back().c_str();
}

void HostImpl::SetConfig(const char* addonId, const char* key, const char* value) {
    ConfigManager::Instance().Set(Text(addonId), Text(key), Text(value));
}

void HostImpl::SaveConfig() { ConfigManager::Instance().Save(); }

uint32_t HostImpl::GetHostVersion() { return EAM_API_VERSION_INT; }

// ---- events

void HostImpl::SubscribeEvent(uint32_t eventId, EamEventCallback callback, void* userData) {
    EventBus::Instance().Subscribe(eventId, callback, userData);
}

void HostImpl::UnsubscribeEvent(uint32_t eventId, EamEventCallback callback) {
    EventBus::Instance().Unsubscribe(eventId, callback);
}

void HostImpl::PublishEvent(uint32_t eventId, const void* data, uint32_t dataSize) {
    EventBus::Instance().Publish(eventId, data, dataSize);
}

// ---- Direct3D 11

void* HostImpl::GetD3D11Device() { return m_d3d11Device; }
void* HostImpl::GetD3D11DeviceContext() { return m_d3d11Context; }

void HostImpl::SetD3D11Device(void* device, void* context) {
    m_d3d11Device = device;
    m_d3d11Context = context;
}

void* HostImpl::GetCurrentComputeShader() { return D3D11Hook::GetCurrentComputeShader(); }
uint32_t HostImpl::GetDispatchCount() { return D3D11Hook::GetDispatchCount(); }
void* HostImpl::GetDispatchingContext() { return D3D11Hook::DispatchingContext(); }
void* HostImpl::CreateImage(const void* rgba, uint32_t width, uint32_t height, uint32_t pitch) { return MakeImageTexture(rgba, width, height, pitch); }
void HostImpl::ReleaseImage(void* image) { if (image) static_cast<IUnknown*>(image)->Release(); }

// ---- dispatch callbacks

void HostImpl::SetPreDispatchCallback(EamPreDispatchCallback callback, void* userData) {
    if (t_inDispatchCallback) { LOG_ERROR("Host", "SetPreDispatchCallback called from inside a dispatch callback: refused (it would deadlock); set callbacks from AddonInitialize, AddonShutdown or another thread"); return; }
    std::lock_guard<std::mutex> lock(m_dispatchMutex);
    SetHook(m_preHooks, callback, userData, ModuleAt(_ReturnAddress()));
    Recount();
}

void HostImpl::SetPostDispatchCallback(EamPostDispatchCallback callback, void* userData) {
    if (t_inDispatchCallback) { LOG_ERROR("Host", "SetPostDispatchCallback called from inside a dispatch callback: refused (it would deadlock); set callbacks from AddonInitialize, AddonShutdown or another thread"); return; }
    std::lock_guard<std::mutex> lock(m_dispatchMutex);
    SetHook(m_postHooks, callback, userData, ModuleAt(_ReturnAddress()));
    Recount();
}

bool HostImpl::InvokePreDispatch(uint32_t x, uint32_t y, uint32_t z) {
    if (!m_preCount.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lock(m_dispatchMutex);
    const InCallback inside;
    bool skip = false, faulted = false;
    for (auto& hook : m_preHooks) {
        bool wantsSkip = false;
        if (!CallPre(hook.callback, x, y, z, hook.owner, &wantsSkip)) { hook.callback = nullptr; faulted = true; continue; }
        if (wantsSkip) skip = true;   // all of them run, even after one asks to skip
    }
    if (faulted) { DropFaulted(m_preHooks, "pre"); Recount(); }
    return skip;
}

void HostImpl::InvokePostDispatch(uint32_t x, uint32_t y, uint32_t z) {
    if (!m_postCount.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(m_dispatchMutex);
    const InCallback inside;
    bool faulted = false;
    for (auto& hook : m_postHooks)
        if (!CallPost(hook.callback, x, y, z, hook.owner)) { hook.callback = nullptr; faulted = true; }
    if (faulted) { DropFaulted(m_postHooks, "post"); Recount(); }
}

// ---- live status and metrics

void HostImpl::SetStatus(const char* addonId, const char* text, int level) {
    Metrics::Instance().SetStatus(Text(addonId), Text(text), level);
}

void HostImpl::PublishMetric(const char* addonId, const char* key, double value, const char* unit) {
    Metrics::Instance().Publish(Text(addonId), Text(key), value, Text(unit));
}

size_t HostImpl::ForgetCode(uintptr_t begin, uintptr_t end) {
    std::lock_guard<std::mutex> lock(m_dispatchMutex);
    const auto inside = [&](auto& hook) { const uintptr_t at = reinterpret_cast<uintptr_t>(hook.callback); return at >= begin && at < end; };
    const size_t before = m_preHooks.size() + m_postHooks.size();
    m_preHooks.erase(std::remove_if(m_preHooks.begin(), m_preHooks.end(), inside), m_preHooks.end());
    m_postHooks.erase(std::remove_if(m_postHooks.begin(), m_postHooks.end(), inside), m_postHooks.end());
    Recount();
    return before - (m_preHooks.size() + m_postHooks.size());
}

size_t HostImpl::DispatchHookCount() {
    std::lock_guard<std::mutex> lock(m_dispatchMutex);
    return m_preHooks.size() + m_postHooks.size();
}

} // namespace eam
