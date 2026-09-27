// PresentHook: sees every frame Lossless Scaling presents (real and generated) just before it goes to the screen, by patching Present and
// Present1 in the function table that every window swap chain of the DXGI runtime shares. The table is found through a short-lived swap chain of
// our own on the same device.
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <functional>

namespace PresentHook {
    using LogFn = std::function<void(const char*)>;
    // Called before the original Present, on the presenting thread, with that present's sync interval and flags. Not called for a
    // DXGI_PRESENT_TEST present.
    using Callback = void (*)(IDXGISwapChain* sc, UINT sync, UINT flags);
    bool Install(ID3D11Device* dev, Callback cb, LogFn log);
    void Uninstall();
    bool Installed();
    unsigned Hits();             // presents seen in the process, by anyone
    // A present of our own, from inside the callback (a frame of our own before Lossless Scaling's): the original Present, which does not
    // run the callback again.
    HRESULT PresentOriginal(IDXGISwapChain* sc, UINT sync, UINT flags);
    void DumpState(LogFn log);   // diagnostics: the hit count, and whether the patched slots are still ours
}
