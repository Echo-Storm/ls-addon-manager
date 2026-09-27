// Frame generation of our own (a prototype, docs/frame-generation-research.md): with Lossless Scaling's frame generation off, every frame it
// presents is held back half a frame, and a frame made between it and the one before is presented first, in Lossless Scaling's own swap
// chain. The frame between is, for now, the two real frames mixed half and half (step 1: the presenting and the pacing); AMD's FSR 3.1 frame
// generation, fed the motion estimate, takes its place in step 2.
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <functional>
#include <string>

namespace nr::framegen {

using LogFn = std::function<void(const char*)>;
// On Lossless Scaling's presenting thread, before it presents a real frame into sc (its output swap chain): presents the frame between
// (through PresentHook::PresentOriginal) and waits until the real frame is due. False when it did nothing (the first frame, a size change,
// a format it cannot take); the real frame goes out as usual either way.
bool BeforeRealPresent(IDXGISwapChain* sc, UINT sync, UINT flags, const LogFn& log);
void Reset();      // forgets the frame before (switched off, a new swap chain)
void Shutdown();   // lets the textures go
struct Stats { uint64_t real = 0, generated = 0; double realIntervalMs = 0, generatedMs = 0; };
Stats GetStats();

} // namespace nr::framegen
