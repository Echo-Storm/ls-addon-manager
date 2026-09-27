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

## Motion vectors refined along depth and colour edges: no gain (2026-09-27)

`nr_fgeval refine=depth|colour|both`: each pixel's vector re-taken from a 5x5 neighbourhood (4 px apart), weighted by likeness in model
depth and colour (a cross-bilateral filter of the motion field). Average scores moved -0.1 to -0.2 dB (hard recording 20.71 -> 20.49 to 20.53;
easier 33.38 -> 33.27 to 33.29), though FSR beat both baselines in 48 of 49 frames instead of 44 on the hard one. The worst frames are not
edge failures: in fast camera turns (30 fps worth of motion between the kept frames) the whole scene is misplaced or doubled, and averaging
vectors adds a little ghosting in branches. Kept as an option, not pursued.

The offline test is harsher than live use: dropping every other frame of a 60 fps recording leaves 30 fps motion to bridge, where frame
generation from 60 to 120 fps bridges half of it. Recordings at 120 fps or more (a lighter game or lower settings) would test the 60 -> 120
case directly.

## First live test, and the turn ghosting (2026-09-27)

The prototype (step 2: FSR 3.1 frame generation fed our motion, in Lossless Scaling's own swap chain) ran live in Silent Hill f, HDR,
3840x2160 output, about 30 real frames a second: FSR made every frame between, no fallbacks. The visible flaw is ghosting in fast camera
turns. Nine HDR recordings of it (2560x1440, 30 fps, NIS's input) were scored with `nr_fgeval`; `tools/fgeval_compare.py` pairs two runs.

- **The estimate was given raw scRGB in HDR** (the live path; offline tests only ever fed it the SDR view). `FlowEstimator::Record` now takes
  the frame's encoding and measures an HDR frame in its SDR view. `nr_fgeval estimate=light|lightview` rebuilds scRGB from a recording to
  compare: 43.86 -> 43.96 dB on one clip. Right, but not the ghosting.
- **The whole picture's shift** (a new pass: every shift within +-20 px at the smallest size, about +-640 px at full size; the best one seeds
  the search's first size, and "not moving" wins unless a shift is clearly better, or a still repeating pattern reads as moving). The
  estimate reaches 80-210 px between kept frames in the turns. Average over the three hardest clips: +0.00, +0.06, +0.22 dB; the worst frame
  12.84 -> 13.38. A small gain: range is not the main problem.
- **What the ghosting is:** the character stays put while the background swings past (a third-person camera), and in the frame between FSR
  lets the background win where they overlap: leaves pasted over the head, hair dragged sideways, a haze beside the character. PSNR over
  frames full of foliage barely registers it.
- **Depth from the motion** (`nr_fgeval depth=motion [dr=N]`: whatever moves unlike the picture's median vector is near): no measurable
  change (20.75 -> 20.64 dB), and by eye mixed: the patch on the head smaller in some frames, speckles in the sky where lone foliage vectors
  come forward. Not pursued as it stands.
- **Keeping what stays put** (`nr_fgeval keepstill=N`: where the two real frames are alike, the frame between is their mix): no help. The
  character is not still on screen in a turn (it shifts a few pixels, and the hair's brightness changes by 15 levels or more as the backlight
  moves), so a threshold on likeness does not find it. Would still suit a HUD; not pursued for the character.
- About half the frames in these turns are below the blend, even with FSR. The offline test bridges two real frames (66 ms at 30 fps),
  twice the gap live; but live at 30 real frames a second is still a large gap, and the most direct improvement is more real frames (the
  frame-rate target idea, deferred).

## Next

1. **The character in turns.** A measure that sees it (the error in a band around what moves unlike the background, instead of the whole
   frame), then: the character as a layer of its own (its motion measured apart from the background's at the edge, where today's 8x8
   blocks straddle both), or a disocclusion mask of our own over FSR's result.
2. **More real frames.** Every gap to bridge halves at 60 real frames a second; the frame-rate target idea (deferred) matters as much as
   anything above.
3. **Depth for the upscalers.** DLSS, FSR and XeSS upscaling take depth (to follow the right object's motion at an edge, to throw history
   away where something is uncovered) and get a flat one today. Measure with an offline upscaler test: a recorded frame shrunk, upscaled
   back with flat and with model depth, scored against the original.
4. **The cost.** The model took about 0.45 s a frame through Python and DirectML at 518 px; for live use it needs to run natively, at a lower
   resolution, and not on every frame (depth changes slowly), with the frames between warped along the motion.
5. **The prototype against LSFG**, side by side live (the prototype runs: see above).

Depth from the game itself (ReShade's Generic Depth add-on, and forks such as PatchedReShade that lift ReShade's block on depth in online
games) stays an optional extra at most: official ReShade switches depth off when a game uses the network, which rules out online games such
as World of Warcraft, and injection carries anti-cheat risk. References: Marty's depth guide (guides.martysmods.com/reshade/depth),
github.com/AldogPlays/PatchedReShade, github.com/Hacktank/ReshadeSansDepthBufferLock.
