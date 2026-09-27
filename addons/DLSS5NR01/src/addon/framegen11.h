// Frame generation of our own (a prototype, docs/frame-generation-research.md): with Lossless Scaling's frame generation off, every frame it
// presents is held back half a frame, and a frame made between it and the one before is presented first, in Lossless Scaling's own swap
// chain. The frame between is made by AMD's FSR 3.1 frame generation fed this project's motion estimate (FgEngine, on a Direct3D 12 device
// of its own); when it cannot make one, the two real frames mixed half and half stand in.
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <functional>
#include <string>

namespace nr::framegen {

using LogFn = std::function<void(const char*)>;
// Each frame as it goes out (the recorder's "what is shown"): the back buffer holding it, on Lossless Scaling's device; made: a frame between.
using ShownFn = std::function<void(ID3D11DeviceContext*, ID3D11Texture2D*, bool made)>;
// On Lossless Scaling's presenting thread, before it presents a real frame into sc (its output swap chain): presents the frame between
// (through PresentHook::PresentOriginal) and waits until the real frame is due. False when it did nothing (the first frame, a size change,
// a format it cannot take); the real frame goes out as usual either way.
// encoding, whiteNits: what its frames hold (0 SDR, 1 scRGB, 2 HDR10; hdr.h) and the SDR white, for measuring their motion. shown (may be
// empty): told of each frame as it goes out, the frame between and then the real one.
bool BeforeRealPresent(IDXGISwapChain* sc, UINT sync, UINT flags, uint32_t encoding, float whiteNits, const LogFn& log, const ShownFn& shown = {});
void SetGuard(bool on);   // the guard against pasted background (FgEngine::Generate); on by default
void SetRuntime(const std::wstring& amdFidelityFxDll);   // AMD's runtime FgEngine loads (the FSR Upscaler's shipped FSR 3.1)
void Reset();      // forgets the frame before (switched off, a new swap chain)
void Shutdown();   // lets the textures go
struct Stats { uint64_t real = 0, generated = 0, byFsr = 0; double realIntervalMs = 0, generatedMs = 0, fsrMs = 0; std::string engine; };
Stats GetStats();

} // namespace nr::framegen
