# Frame generation of our own: research notes

Working notes for the roadmap's "frame generation of our own": what was measured, what was decided, and what is next. Numbers come from
`nr_fgeval` (tools/README.md) on recordings of Silent Hill f (2560x1440, HDR, made with Lossless Scaling's frame generation off), with every
other frame dropped and rebuilt, so the generator bridges 30 fps worth of motion. Scores are PSNR against the real frame (higher is closer;
1 dB is a clear difference).

## Which generator (2026-09-27)

13 recordings, 608 rebuilt frames, each generator fed this project's motion estimate and a flat depth:

| Generator | Average | Closest of the four |
|---|---|---|
| AMD FSR 3.1 frame generation | **30.4 dB** | 92 % of frames |
| a 50/50 blend of the two real frames | 26.1 dB | |
| Intel XeSS frame generation | 25.4 dB | 40 % of frames |
| the frame before, repeated | 24.3 dB | |

**Decision: build on FSR 3.1.** XeSS frame generation is built around the game's camera matrices and real depth, which Lossless Scaling
does not have; FSR's works from motion vectors, which our estimate supplies. FSR's failures are at the edges of things moving against their
background (a character's head against sky and leaves): the edge breaks into fragments.

Lossless Scaling's own frame generation (LSFG) cannot run outside Lossless Scaling, so it is not in this table; whether ours beats it has to
be judged live, side by side.

How each generator has to be driven (both at present time, on a swap chain of their own):
- FSR 3.1: FidelityFX's frame generation swap chain (`FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12`) and a `frameGenerationCallback`; without a
  swap chain its configuration fails (code 3, runtime error). The generated frame can be taken in the callback. OptiScaler sets it up the
  same way (read for reference only: it is GPL-3.0, this project is MIT).
- XeSS: `libxess_fg` on the application's swap chain, with XeLL (`libxell`) in low-latency mode, without which generation stays off; its
  pipelines built up front; its frames only reachable at the swap chain's Present, and copied on the queue that swap chain presents with.

## Does depth help FSR frame generation? No (2026-09-27)

Depth Anything V2 Small (Apache 2.0; `tools/fetch_depth_model.ps1`, `tools/depth_maps.py`) gives convincing depth from the picture alone
(the character near, the sky far, the trees layered between). Fed to FSR 3.1 frame generation in place of the flat depth, on the same 60
frames of the hard recording: model depth 22.65 dB, model depth the wrong way round 22.74, random noise 22.75, flat 22.79. **FSR 3.1 frame
generation makes no real use of depth here.**

## What it does rely on: our motion vectors

The same recordings with zero motion vectors (FSR falls back on its own optical flow): 18.67 dB against 20.71 with ours (hard recording),
30.48 against 33.38 (easier one). Our motion estimate gives most of FSR's lead over a blend, so **better motion vectors are better generated
frames.**

## Next

1. **Motion vectors that follow object edges.** Depth edges are motion edges: the model's depth map says where a character ends and the
   background begins, which is exactly where the estimate's 4x4 blocks blur the motion and FSR breaks the edge. Refine the vectors along
   depth (and colour) edges; measure with `nr_fgeval`.
2. **Depth for the upscalers.** DLSS, FSR and XeSS upscaling take depth (to follow the right object's motion at an edge, to throw history
   away where something is uncovered) and get a flat one today. Measure with an offline upscaler test: a recorded frame shrunk, upscaled
   back with flat and with model depth, scored against the original.
3. **The cost.** The model took about 0.45 s a frame through Python and DirectML at 518 px; for live use it needs to run natively, at a lower
   resolution, and not on every frame (depth changes slowly), with the frames between warped along the motion.
4. **A prototype in the addon**, for a live comparison with LSFG.

Depth from the game itself (ReShade's Generic Depth add-on, and forks such as PatchedReShade that lift ReShade's block on depth in online
games) stays an optional extra at most: official ReShade switches depth off when a game uses the network, which rules out online games such
as World of Warcraft, and injection carries anti-cheat risk. References: Marty's depth guide (guides.martysmods.com/reshade/depth),
github.com/AldogPlays/PatchedReShade, github.com/Hacktank/ReshadeSansDepthBufferLock.
