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

## The first live test: turn ghosting (2026-09-27)

The prototype (FSR Upscaler, Silent Hill f, 4K HDR, 30 fps real) ran with FSR 3.1 making every frame between (no fallback to the blend in
the log), and showed ghosting in fast camera turns. Nine recordings of it (1440p HDR, 30 fps) went through `nr_fgeval`:

- **The motion estimate keeps track.** In a turn of 55-70 px between kept frames (every other frame dropped), the frame before moved by our
  vectors matches the next one at 26-30 dB against 18-23 dB not moved (`mvcheck=1`); the search reaches about 190 px at 1440p.
- **The scores stop telling.** FSR, a plain blend, both frames moved half way along our vectors, and a smarter half way (each pixel choosing
  among its neighbours' vectors where the two frames agree, one-sided where they do not) all land within 0.3 dB, even at a quarter of the size
  (`PsnrCoarse`). Yet the pictures differ plainly: FSR's frame in a turn is sharp and in place, the blend a double image. In dense foliage a
  leaf a pixel off costs as much as a ghost, so **look at the pictures** before trusting a fraction of a dB.
- **Not the vectors' length, not depth.** Vectors scaled by 0.5 help one recording and hurt another (0 to 1.0 all within 0.6 dB); a depth made
  from the motion (what moves unlike the picture as a whole is near: the third-person character) changes nothing (FSR 3.1 frame generation
  takes little from depth, as before). Uneven frame times are not it either (33.3 ms apart throughout).
- **Live cadence looks clean.** `live=1` makes a frame between every two frames of a recording, as live: in the turn, FSR's frames are sharp,
  in the right place, one speck at the character's hair.
- **Fixed in the live path:** (1) the frame between went out as soon as it was made and the real frame half a frame after arrival, so with
  5-10 ms of making at 30 fps they were about 11 and 22 ms apart (uneven: judder, read as doubling in a turn). Both now go out relative to
  arrival, the frame between after the recent peak of making time. (2) HDR frames reached the motion estimate as light (scRGB) instead of
  their SDR view, which its thresholds are made for; now in their SDR view (no measurable change on these dark recordings). (3) What frame
  generation did is logged every 10 s.
- **Still to rule out:** the FSR Upscaler's own history smearing in fast turns (the recordings are taken before it, so none of this shows it).
  A live check: frame generation with the FSR Upscaler off (NIS only).


Also on 2026-09-27 (a second session, on the same recordings; `tools/fgeval_compare.py` pairs two `nr_fgeval` runs frame by frame):

- **The whole picture's shift** seeds the search now (a pass trying every shift within +-20 px at the smallest size, about +-640 px at full
  size; "not moving" wins unless a shift is clearly better, or a still repeating pattern reads as moving, which the test host caught). The
  turns reach 80-210 px between kept frames; averages over the three hardest clips +0.00, +0.06, +0.22 dB, the worst frame 12.84 -> 13.38.
  A small gain: range is not the main problem.
- **What the ghosting looks like** (the worst frame, cropped): leaves from the background pasted over the character's head, the hair
  dragged sideways, a haze beside it; the character stays near put while the background swings past, and FSR lets the background win where
  the two overlap.
- **Keeping what stays put** (`keepstill=N`: where the two real frames are alike, the frame between is their mix): no help, because the
  character is not still on screen in a turn (it shifts a few pixels, and the backlit hair's brightness changes by 15 levels or more).
  Would suit a HUD.

## Turn ghosting, found: the upscalers' history (2026-09-27, afternoon)

`nr_sreval` (new, `addons/DLSS5NR01/tools/sr_eval.cpp`) runs the addon's own upscaler engine offline: each recorded frame shrunk 1.5x (as
1440p to 4K), the motion estimated, upscaled back by FSR 3.1, DLSS or XeSS, and compared with the frame itself beside a plain bilinear
stretch. Scores in full and at a quarter of the size (where things are, not their detail).

- **Every upscaler trails in a fast turn.** Silent Hill f, a 30-frame turn: FSR 39.4, DLSS 36.8, XeSS 39.1 dB at a quarter of the size,
  against 43.3 for the plain stretch. After a reset the upscaler matches the stretch (48.3 against 49.1) and then falls behind frame by
  frame as its history builds: the leaves get soft doubled edges. Without our vectors it is far worse (30.2), so the vectors are not the
  fault; the history is. Lossless Scaling's frames carry no sub-pixel jitter, so a temporal upscaler's history adds little detail here and
  in fast motion mostly trails.
- **The fix: lean on the frame in fast motion.** The distrust mask now also rises with the motion itself (from 0.5 % of the frame's width a
  frame, fully at twice that; `kFastMotionShare` in sr_engine.cpp). FSR 39.4 -> 41.8 dB in the turn, the doubled edges gone; slow motion
  untouched. XeSS now gets the mask too, as its responsive pixel mask (39.1 -> 40.2).
- **DLSS ignores the mask.** Its bias-current-colour mask changes nothing (36.8 with and without): the DLSS Upscaler has never used our
  distrust. Open.
- **A slow drift remains** in slow pans for FSR and DLSS (not XeSS): 49 -> 44 dB at a quarter of the size over 40 frames, in foliage
  detail. Not motion error (scaling the vectors 0.8 to 1.2 changes nothing). Open.

## The second live test: frame generation halved the real frame rate (2026-09-27, evening)

"Record what is shown" (the presented frames, tagged made / real) worked: each frame between sits between its two real frames (no pairing
bug). But the real frames came 50-60 ms apart (about 18 a second, against 30 before), and FSR took about 20 ms a frame between. Two pacers fed
each other: FidelityFX's frame generation swap chain paces its own presents (half the frame time between its frame between and the real
one) and blocks in Present until it has, and our hook then held Lossless Scaling's thread half the measured interval more. Held for most of a
frame, Lossless Scaling's loop slowed, the measured interval grew, and so did both waits: interval = 2 x (FSR's wait + Lossless Scaling's work).

Fixed: FSR 3.1 frame generation is dispatched directly on our own command list (configured with FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY,
then configure, prepare and dispatch with one frame ID; AMD's provider source shows this path), with no swap chain or hidden window; the
engine never waits for the GPU (Lossless Scaling's queue waits on a shared "made" fence before copying the frame between in); pacing uses the
measured GPU time (3.4 ms at 1440p, motion estimate included; the same frames as before, `nr_fgeval engine=1` against `direct=1`). The log
now says how long Lossless Scaling's thread is held and how soon it comes back. Still to do: a presenter of our own (the hold for half a frame
is still on Lossless Scaling's thread; the clean way is replacement back buffers, as FidelityFX's own swap chain does).

## The upscalers in fast turns: the lean pass (2026-09-27, night)

DLSS 4's transformer presets (J, K, M) ignore the bias-current-colour mask entirely (nr_sreval preset=N, mask on and off: identical); the
old CNN preset E reads it but scores worse with it. So a pass of our own after every upscaler blends its picture toward this frame upscaled
plainly (Catmull-Rom), by the distrust mask (untrusted motion, and fast motion from 0.5 % of the width a frame). A 30-frame turn shrunk
1.5x, at a quarter of the size (plain stretch 43.3 dB): DLSS 36.8 -> 44.2, FSR 3.1 41.7 -> 45.6 (on top of its reactive mask), XeSS
40.2 -> 46.2; full scores all above the stretch too (32.6 / 32.9 / 33.0 against 32.0); a slow pan unchanged. Under "Steady in fast motion".
Preset J beats K in turns (37.6 against 36.8 before the lean). Open: the slow-pan drift of FSR and DLSS (not XeSS) in foliage.

## FSR 3.1 frame generation from the inside (AMD's MIT-licensed source, FidelityFX SDK)

- The frame between is built by scattering each pixel of the newer frame half way along its vector; where two land on one pixel, the
  priority is (high bits) the view-space depth, nearer wins, then (low bits) how well its colour matched along the vector. With our flat
  depth the colour match decides, and in a turn the background often wins over a character: leaves pasted over the head.
- Its own optical flow reaches only small motion: in a 170 px turn it reads zero everywhere (debug view), so its fallback is a plain
  blend, weighted in where the colours along our vectors agree less than a blend's do (haze).
- `nr_fgeval debugview=1` draws FSR's debug views (game vectors | depth priority | optical flow / disocclusion | source | backbuffer);
  `ffxdebug=1` its messages. The runtime we ship holds frame generation 1.1.3 (the camera info of 3.1.4 does not apply).
- **Depth from an orbiting camera** (`depth=orbit`): how far a block moves along the turn's direction orders it in depth (the character at
  the pivot moves least, the far scene most). FSR's priority then shows the character in front (debug view), but the frames between at the
  live gap change little; not carried into the addon yet. `depth=layer` (two layers by departure from a fitted camera motion) marks too much:
  in an orbit the scene's motion itself varies with depth.
- **The motion estimate on a character in a turn**: its cost of straying from the coarser guess was unbounded, so in a 170 px turn a dark,
  faintly textured character took random vectors; it is now capped at 2 px of each size (`FlowEstimator::SetStrayCap`). The character's
  own motion stays ambiguous in an orbit (it turns in 3D, walks), which no vector captures.
- `nr_fgeval` also scores a **band** around what moves unlike the camera (the character and a margin), where whole-frame scores could not
  see the ghosting; `worstby=band`, `mvdump=1` (the vectors as a picture), `live=1` for the live gap.

## Next

1. **Live check of the upscaler fix** (fast motion leaning on the frame): the same turns with frame generation on, then with the FSR
   Upscaler off (NIS only), to see what trailing is left and whose. Then DLSS's ignored mask, and the slow drift.
2. **The character in turns.** A measure that sees it (the error in a band around what moves unlike the background, not the whole frame),
   then: the character as a layer of its own (its motion measured apart from the background's at the edge, where today's 8x8 blocks
   straddle both), or a disocclusion mask of our own over FSR's result.
3. **More real frames.** Every gap to bridge halves at 60 real frames a second; the frame-rate target idea (deferred) matters as much as
   anything above.
4. **Depth for the upscalers.** DLSS, FSR and XeSS upscaling take depth (to follow the right object's motion at an edge, to throw history
   away where something is uncovered) and get a flat one today. Measure with an offline upscaler test: a recorded frame shrunk, upscaled
   back with flat and with model depth, scored against the original.
5. **The cost.** The model took about 0.45 s a frame through Python and DirectML at 518 px; for live use it needs to run natively, at a lower
   resolution, and not on every frame (depth changes slowly), with the frames between warped along the motion.
6. **The prototype against LSFG**, side by side live (the prototype runs: see above).

Depth from the game itself (ReShade's Generic Depth add-on, and forks such as PatchedReShade that lift ReShade's block on depth in online
games) stays an optional extra at most: official ReShade switches depth off when a game uses the network, which rules out online games such
as World of Warcraft, and injection carries anti-cheat risk. References: Marty's depth guide (guides.martysmods.com/reshade/depth),
github.com/AldogPlays/PatchedReShade, github.com/Hacktank/ReshadeSansDepthBufferLock.

## How LSFG 3.1 works (lsfg-vk study)

See [lsfg-vk-study.md](lsfg-vk-study.md): LSFG's pass graph as lsfg-vk runs it (features at 7 levels, coarse-to-fine motion, refinement
at the three finest levels, its own synthesis pass), what we lack, and the licence line (read and learn; copy nothing).
