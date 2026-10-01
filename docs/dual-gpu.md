# Running the model on a second graphics card (research, 2026-10-01)

The idea (docs/magpie-dissection.md, "Dual GPU"): when the card Lossless Scaling runs on is full (the Durotar spot: the game falls from 60 to about 35 fps with Neural Rendering on, and the
model waits 6 to 22 ms for the card), run the model on an idle second card and send it frames. Nobody does this (the Magpie fork keeps everything on one adapter). Issue #6 asked for it
indirectly. Today the model runs on the card Lossless Scaling runs on.

## What was tried: the plumbing, with `nr_xadapter` (tools/xadapter_test.cpp)

One card here (RTX 4070 Ti SUPER), so the second adapter is WARP (Microsoft's software adapter). That proves the API calls and the ordering, not the bus speed.

What works (a 66 MB buffer, the size of a 4K half-float frame, copied first card to second, pixels checked, 30 runs, 0 wrong):

- A **heap** made on the first device with `D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER`, type DEFAULT, **no other flags**; a shared handle for it; `OpenSharedHandle` on the second device;
  a buffer (`ROW_MAJOR`, `D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER`) **placed** in it on each device (`CreatePlacedResource`).
- A **fence** made with `D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER`, shared the same way: the first card's queue signals it after its copy, the second's queue waits on it.
- This GeForce reports `CrossAdapterRowMajorTextureSupported = no` and still accepts the shared buffer heap: that flag is about cross-adapter *textures*; buffers are the route.

What does not (found with the debug layer, so the next person need not):

- A *committed* buffer with `SHARED_CROSS_ADAPTER` ("the resource dimension must be TEXTURE2D"): use a heap and a placed buffer.
- `ALLOW_ONLY_BUFFERS` on that heap (it sets the DENY flags, which a cross-adapter heap may not have).
- UPLOAD, READBACK and GPU_UPLOAD heaps; `D3D12_HEAP_FLAG_SHARED` on a CPU-accessible heap; a CUSTOM heap other than `NOT_AVAILABLE` + `L0`.

WARP's side of the copy runs at about 0.01 GB/s (a software path), so its time says nothing.

## What a real design needs (not built)

1. **Pick the second card**: another hardware adapter than Lossless Scaling's (`g_engineCard` already says which card the engine uses; today it follows LS's). A setting "Run the model on: LS's card / <card>".
2. **Frames over**: a copy on LS's card from the captured frame (D3D11, `Bridge`'s shared textures) into the cross-adapter buffer, then a signal; the model's card waits, copies into its own texture (a 4K half-float frame is 66 MB, 4 GB/s
   at 60 a second, which fits PCIe 4.0 x16; half that size at the model's working scale if the shrink is done first on LS's card, which also costs less bus).
3. **Deltas back**: the model's output at the working scale (small) copied the other way the same way, then composed on LS's card as now. The result is consumed one frame late already, so the added latency
   (two copies, about 1 to 3 ms) is hidden by the compose's own late-by-a-frame scheme.
4. **Failure paths**: a card that disappears (a laptop's discrete card powering down), a second card slower than the first, differing formats; each falls back to LS's card.
5. **The NGX model on the other device**: `NrEngine` already takes a `LUID`; the engine would start on the second card and its own D3D12 queue (it is D3D12 already; only the Bridge's D3D11 interop on LS's card changes).
6. **Numbers to take first, on two real cards**: `nr_xadapter` with `first=<index> second=<index>` for the bus rate and the latency of a round trip; then whether the whole path beats the single-card path at the Durotar spot.

Worth doing only for someone with two capable cards (or an integrated card that is no use for the model): so a request from a user with such a setup (issue #6's reporter was asked for their cards) decides when.

## Using the tool

```
nr_xadapter [runs=30] [mb=66] [first=<adapter index>] [second=<adapter index>|warp] [debug=1]
```

It lists the adapters, makes the shared heap, fence and buffers, copies and checks, and prints the time of a copy. `debug=1` turns on the D3D12 debug layer and prints its messages when the heap is refused.
