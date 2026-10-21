#include "shader_hook.h"
#include "hook_util.h"
#include "../log/logger.h"
#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ShaderHook {

namespace {

// Our handles: this tag in the upper half, a sequence number in the lower. Real resource handles point into a loaded module, which never has
// the tag's address.
constexpr uintptr_t kTag = uintptr_t(0xF00DB00F) << 32;
constexpr uintptr_t kTagMask = uintptr_t(0xFFFFFFFF) << 32;
bool IsOurs(const void* handle) { return (reinterpret_cast<uintptr_t>(handle) & kTagMask) == kTag; }

struct Replacement { std::vector<uint8_t> bytes; };

std::mutex g_mutex;
Interceptor g_intercept;
// Every replacement ever handed out, never removed: Lossless Scaling may keep the pointer LockResource gave it.
std::unordered_map<uintptr_t, std::unique_ptr<Replacement>> g_replacements;
std::unordered_map<std::wstring, uintptr_t> g_handleFor;   // the same resource with the same bytes gets the same handle again
uint32_t g_nextSerial = 1;

using FindResourceWFn = HRSRC(WINAPI*)(HMODULE, LPCWSTR, LPCWSTR);
using LoadResourceFn = HGLOBAL(WINAPI*)(HMODULE, HRSRC);
using SizeofResourceFn = DWORD(WINAPI*)(HMODULE, HRSRC);
using LockResourceFn = LPVOID(WINAPI*)(HGLOBAL);
using FreeResourceFn = BOOL(WINAPI*)(HGLOBAL);

// A patched import slot and what it held before.
struct Patched { const char* name; void* hook; void** slot = nullptr; void* original = nullptr; };

HRSRC WINAPI OnFindResourceW(HMODULE, LPCWSTR, LPCWSTR);
HGLOBAL WINAPI OnLoadResource(HMODULE, HRSRC);
DWORD WINAPI OnSizeofResource(HMODULE, HRSRC);
LPVOID WINAPI OnLockResource(HGLOBAL);
BOOL WINAPI OnFreeResource(HGLOBAL);

Patched g_patched[] = {
    { "FindResourceW", (void*)&OnFindResourceW }, { "LoadResource", (void*)&OnLoadResource }, { "SizeofResource", (void*)&OnSizeofResource },
    { "LockResource", (void*)&OnLockResource }, { "FreeResource", (void*)&OnFreeResource },
};
template <class Fn> Fn Original(int i) { return reinterpret_cast<Fn>(g_patched[i].original); }

std::wstring KeyOf(LPCWSTR name, LPCWSTR type) {
    auto part = [](LPCWSTR s) { return IS_INTRESOURCE(s) ? L"#" + std::to_wstring((uintptr_t)s) : std::wstring(s ? s : L""); };
    return part(type) + L"/" + part(name);
}

const Replacement* Find(const void* handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_replacements.find(reinterpret_cast<uintptr_t>(handle));
    return it == g_replacements.end() ? nullptr : it->second.get();
}

HRSRC WINAPI OnFindResourceW(HMODULE module, LPCWSTR name, LPCWSTR type) {
    const void* data = nullptr; uint32_t size = 0;
    Interceptor intercept;
    { std::lock_guard<std::mutex> lock(g_mutex); intercept = g_intercept; }
    if (intercept && intercept(name, type, &data, &size) && data && size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        std::lock_guard<std::mutex> lock(g_mutex);
        const std::wstring key = KeyOf(name, type);
        const auto known = g_handleFor.find(key);
        if (known != g_handleFor.end()) {
            const std::vector<uint8_t>& had = g_replacements[known->second]->bytes;
            if (had.size() == size && std::equal(had.begin(), had.end(), bytes)) return reinterpret_cast<HRSRC>(known->second);
        }
        const uintptr_t handle = kTag | g_nextSerial++;
        g_replacements[handle] = std::make_unique<Replacement>(Replacement{ std::vector<uint8_t>(bytes, bytes + size) });
        g_handleFor[key] = handle;
        LOG_DEBUG("ShaderHook", "Resource %s replaced by an addon (%u bytes)", std::filesystem::path(key).u8string().c_str(), size);
        return reinterpret_cast<HRSRC>(handle);
    }
    {   // (debug log level) which resources Lossless Scaling asks for, each once: shows what a scaling type or a frame generation mode uses
        static std::unordered_set<std::wstring> seen;
        std::lock_guard<std::mutex> lock(g_mutex);
        const std::wstring key = KeyOf(name, type);
        if (seen.insert(key).second) LOG_DEBUG("ShaderHook", "Resource %s asked for", std::filesystem::path(key).u8string().c_str());
    }
    const auto real = Original<FindResourceWFn>(0);
    return real ? real(module, name, type) : nullptr;
}

HGLOBAL WINAPI OnLoadResource(HMODULE module, HRSRC res) {
    if (IsOurs(res)) return reinterpret_cast<HGLOBAL>(res);   // our handle serves as its own "loaded" handle
    const auto real = Original<LoadResourceFn>(1);
    return real ? real(module, res) : nullptr;
}

DWORD WINAPI OnSizeofResource(HMODULE module, HRSRC res) {
    if (IsOurs(res)) { const Replacement* r = Find(res); return r ? static_cast<DWORD>(r->bytes.size()) : 0; }
    const auto real = Original<SizeofResourceFn>(2);
    return real ? real(module, res) : 0;
}

LPVOID WINAPI OnLockResource(HGLOBAL loaded) {
    if (IsOurs(loaded)) { const Replacement* r = Find(loaded); return r ? const_cast<uint8_t*>(r->bytes.data()) : nullptr; }
    const auto real = Original<LockResourceFn>(3);
    return real ? real(loaded) : nullptr;
}

BOOL WINAPI OnFreeResource(HGLOBAL loaded) {
    if (IsOurs(loaded)) return FALSE;   // what FreeResource returns for success: it is obsolete and frees nothing
    const auto real = Original<FreeResourceFn>(4);
    return real ? real(loaded) : FALSE;
}

} // namespace

bool InstallHooks(HMODULE module, Interceptor intercept) {
    UninstallHooks();
    { std::lock_guard<std::mutex> lock(g_mutex); g_intercept = std::move(intercept); }
    int patched = 0;
    for (Patched& p : g_patched) {
        p.slot = eam::hooks::ImportSlot(module, "kernel32.dll", p.name);
        p.original = p.slot ? eam::hooks::SwapSlot(p.slot, p.hook) : nullptr;
        if (!p.original) { p.slot = nullptr; LOG_WARN("ShaderHook", "%s is not imported from kernel32 (or could not be patched): it is not intercepted", p.name); continue; }
        ++patched;
    }
    LOG_INFO("ShaderHook", "%d of 5 resource functions hooked", patched);
    return patched > 0;
}

void UninstallHooks() {
    for (Patched& p : g_patched) {
        if (p.slot && *p.slot == p.hook) eam::hooks::SwapSlot(p.slot, p.original);   // only while it is still ours
        p.slot = nullptr;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_intercept = nullptr;
}

} // namespace ShaderHook
