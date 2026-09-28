// How many of our engines use NGX in this process (the DLSS Upscaler, Neural Rendering and DLAA can run side by side in Lossless Scaling).
// NGX is shared by the whole process: one engine's NVSDK_NGX_D3D12_Shutdown1, even on its own device, took the other's feature away
// ("FeatureNotFound" at its next evaluate; the Neural Rendering addon switched off while the DLSS Upscaler ran, 2026-09-27). So only the
// last one out shuts NGX down. The count lives in a small named mapping of this process's own, shared by every addon DLL in it.
#pragma once
#include <windows.h>
#include <cstdio>

namespace nr::ngxusers {

inline volatile LONG* Count() {
    static volatile LONG* count = [] {
        wchar_t name[64]; swprintf(name, 64, L"Local\\EchoAddonsNgxUsers-%lu", GetCurrentProcessId());
        HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LONG), name);   // (kept open for the process)
        void* view = map ? MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG)) : nullptr;
        return static_cast<volatile LONG*>(view);
    }();
    return count;
}
// After NGX was started on an engine's device.
inline void Joined() { if (volatile LONG* c = Count()) InterlockedIncrement(c); }
// When an engine lets NGX go: true when it was the last, and may shut NGX down; false when another engine still uses it (it releases its
// own feature and parameters only). Without the count (the mapping failed), true: as before.
inline bool Leaving() { volatile LONG* c = Count(); return !c || InterlockedDecrement(c) <= 0; }
// How many engines use NGX now (0 when unknown).
inline LONG Users() { volatile LONG* c = Count(); return c ? *c : 0; }

} // namespace nr::ngxusers
