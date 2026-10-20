// D3D11Hook: finds Lossless Scaling's D3D11 device, and runs the addons' pre- and post-dispatch callbacks around each of its compute passes.
//
// The device: Lossless_original.dll calls D3D11CreateDevice through its delay-load table; that slot is pointed at ours, which hands every device it
// makes to the host (and publishes the device events).
//
// The passes: a code hook on each ID3D11DeviceContext::Dispatch implementation in d3d11.dll, which runs the callbacks only for the immediate
// contexts of those devices: all of them, since Lossless Scaling makes more than one and its compute passes run on the first. Not a patch of the
// context's function table: each context keeps its own copy of that table, and ID3D11Multithread::SetMultithreadProtected() (which Lossless
// Scaling calls) swaps the copy's entries, dropping a patched slot.
#pragma once
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
namespace eam { class HostImpl; }

namespace D3D11Hook {

// Points Lossless_original.dll's delay import of D3D11CreateDevice at ours. Makes no D3D calls, so it is safe in DllMain.
void Initialize(eam::HostImpl* host);
// Hooks the Dispatch implementations. Creates short-lived D3D11 devices to find them, so it must run outside the loader lock (the manager does it on
// its window thread, before the addons load). Returns how many were hooked.
int InstallDispatchHooks();
// Stops handing out devices and running callbacks. The code hooks stay in place (idle): a hook placed on top of ours later calls through them.
void Shutdown();

// One observer of the manager itself (the recorder) sees every pass before the addons' callbacks, on the same thread; null removes it. It must not skip,
// wait or dispatch. An addon's callbacks are separate (IHost).
using DispatchObserver = void (*)(ID3D11DeviceContext* ctx, uint32_t x, uint32_t y, uint32_t z);
void SetDispatchObserver(DispatchObserver observer);

// What the D3D11CreateDevice hook does with a new device (the tests call it directly).
void Attach(ID3D11Device* device, ID3D11DeviceContext* context);

// The compute shader bound on the context whose Dispatch is running on this thread (borrowed); null outside a dispatch callback.
void* GetCurrentComputeShader();
// Dispatches on Lossless Scaling's contexts since its first device was created.
uint32_t GetDispatchCount();
// The context whose Dispatch is running on this thread; null outside a dispatch callback.
ID3D11DeviceContext* DispatchingContext();

} // namespace D3D11Hook
