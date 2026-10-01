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

## The flicker while moving in World of Warcraft (2026-09-28): DLSS preset L pulses every 4 frames

The owner saw flicker while moving with the DLSS Upscaler at 1:1 (DLAA) in World of Warcraft and recorded 17 clips (wowb_2026-09-27_23-54-*.lsrec,
3840x2160, 36 fps: the frames the upscaler is given). Replayed with `nr_sreval shrink=100` (1:1, as played), clip 23-54-43, frames 40-79:

- The game's frames change evenly from one to the next (`vs before` about 23.7 dB every frame): no repeats, no rhythm of their own.
- DLSS preset L: steadiness 36.3 dB on average, with an exact 4-frame cycle (best at frames 51, 55, 59 ... 79; about +-0.5 dB, 9 times a
  second at 36 fps). With the motion estimate off (`motion=none`) the cycle stays and grows (+-0.8 dB); with the lean pass off (`fast=0`)
  it stays. So it is inside model L: we give DLSS no jitter (0, 0).
- FSR and XeSS: no cycle. DLSS K 32.5, J 32.6: no cycle, far less steady. **DLSS E 35.4: no cycle**, and the closest to the game's
  picture (37.55 dB against L's 36.73), the lightest too.
- Next: the owner tries E live where it flickered. If it is gone, recommend E at 1:1 (DLAA) and keep L for upscaling (its lead there,
  on the Silent Hill f recording, is in slow pans).

`nr_sreval` prints `vs before` per frame now (the frame against the one before; 99: a repeat).

## The upscalers in steady motion: lean from much less when upscaling (2026-09-28)

On the owner's World of Warcraft clip upscaled 1.5x (walking, about 7-8 px a frame at 1440p), every upscaler lost to a plain stretch
(coarse: DLSS L 42.13, FSR 40.67, XeSS 43.53, stretch 44.24): too slow for the lean (from 0.5 % of the width, about 13 px) and fast
enough for the history to trail. Our vectors are good there (`nr_fgeval mvcheck=1`: half way 31-32 dB against 22.7 still), yet DLSS
scores the same with them as without. On the Silent Hill f turn with the lean off, DLSS with our vectors as they are scores *below* no
vectors (coarse 32.78 against 34.32) and below deliberately wrong ones (flipped 37.30, half 36.69, double 37.37): with vectors it
believes, it keeps its history, and without the game's jitter that history only trails; wrong vectors make it drop the history. So the
vectors are handed over right; the history itself is what hurts in motion here.

Hence, when upscaling (not at 1:1), the lean starts at 0.05 % of the width a frame (about 1.3 px at 2560), fully at twice that
(`kFastMotionShareUpscaling`). Silent Hill f turn, 1.5x, before -> after (upscaled / coarse / steady):
DLSS L 32.82 / 43.18 / 30.11 -> 34.02 / 47.17 / 31.13; FSR 33.44 / 45.47 / 30.59 -> 34.10 / 47.84 / 31.20;
XeSS 33.61 / 46.42 / 30.72 -> 34.07 / 47.82 / 31.18 (a plain stretch 32.68 / 43.94 / 29.78). Its slow start (DLSS L): coarse 47.02 -> 48.42.
World of Warcraft (DLSS L): 32.36 / 42.13 / 32.16 -> 32.74 / 43.14 / 32.91 (stretch 32.49 / 44.24 / 32.39).
At 1:1 (DLAA) the threshold stays at 0.5 %: there the reference is the game's own aliased frame, so leaning always scores better
while it gives the aliasing back; the owner is happy with E at 1:1 as it is.

## Neural Rendering's flicker, offline (2026-09-29): `nr_nreval`

`nr_nreval <recording> <folder> [scale=50] [smooth=40] ...` runs the addon's own NrEngine (the user's `nvngx_dlssnr.dll`) on a recording, shared
textures and fences as the addon drives it, and scores what the model adds from frame to frame:
- **steady** (dB): the picture's change from the last one against the game's own change (as `nr_sreval`);
- **still**: over the pixels where the game's frame did not change, the mean change of the model's delta (levels of 255): pure model flicker;
- **along the motion**: the delta against the previous delta moved along the game's real motion (independent block matching), mean and the worst 5 %
  of blocks; a heat map of where it sits (`flicker_heat.bmp`);
- **the live path**: this frame shown with the run before's delta sampled at `uv + motion` (compose11, frame generation off), against the frame's own
  delta, and against no compensation at all.

World of Warcraft walking (Gooseberry Lowlands, night; 4K frames, the model at 1920x1080, 30 frames), by Temporal smoothing:

| smoothing | steady | still | along the motion | live path (not moved) |
|---|---|---|---|---|
| 0 | 41.90 dB | 1.09 | 1.41 | |
| 0.4 | 42.94 | 0.76 | 0.97 (worst 5 %: 1.75) | 1.19 (2.34) |
| 0.6 | 43.57 | 0.60 | 0.75 | |

So smoothing does what it is for (cuts the model's own change frame to frame by about a third at 0.4), and the live path's motion compensation is
as good as an independent block matcher's (1.0 against 0.97 levels in steady walking; worse in the first frames after a start, 1.5 to 2.5).
The model's change itself is large (up to 70 levels: it recolours the ground and invents gravel texture), and the flicker left is about one level of
255 on average: small. The owner's "weird landscape flicker" (2026-09-28/29) is **not in this clip**: the recordings were made with the DLSS Upscaler,
not Neural Rendering, in a night scene. Next: a recording made with Neural Rendering on and its own recorder on (the Save key says which
addon's recorder is off now), in daylight with foliage and terrain, then `nr_nreval` on it (look at the worst 5 % and the heat map).

By the model's working size (same clip, smoothing 0.4; the size does not decide the flicker, only the cost and the detail do):

| working size | steady | still | along the motion (worst 5 %) | live path |
|---|---|---|---|---|
| 0.25 (960x544) | 44.67 dB | 0.74 | 0.89 (1.65) | 1.11 |
| 0.35 (1344x760) | 43.50 | 0.75 | 1.02 (2.05) | 1.16 |
| 0.50 (1920x1080) | 42.84 | 0.76 | 0.97 (1.74) | 1.18 |
| 0.75 (2880x1624) | 43.02 | 0.63 | 0.89 (1.62) | 1.02 |

Offline GPU times are not comparable to the live ones (the tool runs one frame at a time with long gaps; the GPU idles and clocks down: 26 to 61 ms
here against about 8.5 ms live at 0.5). Because the flicker does not depend on the size, Auto quality can go lower (its floor is 0.25 in
`auto_quality.h`; the 4K WoW session sat at its 0.50 with the model at 8.5 ms against a 5 ms budget) without making the model flicker more; what a
smaller size costs is detail in what the model adds, which this score does not measure.

## The upscalers at 1:1 (DLAA) with no reference (2026-09-29): sharpening amplifies the shimmer

At 1:1 the game's own frame is the input and there is no true picture to compare with (the frame is aliased), so `nr_sreval` now also prints two
no-reference scores (levels of 255 on luma), each against the game's own frames: **flicker** (the middle of three frames from the average of its
neighbours: shimmer, plus the real motion, which the game's own frames carry equally) and **detail** (the mean step between neighbouring pixels).
The clips are all in motion, so the game's own flicker is 2 to 13 levels and dominates; the ratio is what counts.

World of Warcraft walking, 1:1, DLSS model E, by the Sharpening slider (the engine scales it by 1.6):

| slider | clip 23-54-01: flicker / detail | clip 23-54-43: flicker / detail |
|---|---|---|
| 0 | 90 % / 95 % | 91 % / 86 % |
| 0.3 | 108 % / 150 % | 117 % / 147 % |
| 0.7 (the owner's) | 137 % / 229 % | 155 % / 235 % |
| 1.0 | 160 % / 286 % | 186 % / 302 % |

Without sharpening model E calms the shimmer a little (about 10 %) and keeps 86 to 95 % of the detail: DLAA without the game's jitter can only smooth, not
add. Sharpening then puts the detail back, and **more than back**: at 0.3 half as much detail again as the game's own picture, at 0.7 more than twice as
much, with the shimmer of the game's aliasing amplified too (108 to 117 % at 0.3, 137 to 155 % at 0.7). "Neutral" (detail and shimmer both as the game's own) is
about 0.1 to 0.15. The other backends at slider 0 on clip 23-54-43: model L 94 % / 84 %, K 88 % / 79 %, FSR 92 % / 85 %, XeSS 90 % / 80 % (flicker / detail).

Ideas: sharpening that follows the temporal stability (less where a pixel changes from frame to frame without moving), so it puts the detail back
without the shimmer; a lower default at 1:1 (0.5 today).

## The upscalers side by side, and how far the lean should go (2026-09-29)

Defaults, the Auto DLSS model, upscaled 1.5x from recordings (`nr_sreval`; upscaled dB / coarse / steady; a plain stretch in the last column):

| clip | DLSS | FSR 3.1 | XeSS | stretch |
|---|---|---|---|---|
| Silent Hill f, slow start | 34.63 / 48.43 / 31.77 | 34.65 / 48.75 / 31.78 | 34.61 / 48.70 / 31.75 | 33.43 / 45.17 / 30.56 |
| Silent Hill f, fast turn | 34.02 / 47.17 / 31.13 | 34.10 / 47.84 / 31.20 | 34.07 / 47.82 / 31.18 | 32.68 / 43.94 / 29.78 |
| World of Warcraft, walking | 32.74 / 43.14 / 32.91 | 32.05 / 42.75 / 32.50 | 32.72 / 44.46 / 32.95 | 32.49 / 44.24 / 32.39 |

The three are tied on Silent Hill f (FSR's old slow-pan drift, 45.6 against 49.8, is gone: the lean took it). On World of Warcraft walking DLSS and FSR are
below a plain stretch on the coarse score. Sweeping where the lean starts (`fastp=`, hundredths of a pixel; the default is 0.64 px at 1280 wide), coarse:

| lean from | DLSS | FSR | XeSS |
|---|---|---|---|
| default (0.64 px) | 43.14 | 42.75 | 44.46 |
| 0.30 px | 43.37 | 43.60 | 44.71 |
| 0.05 px | 43.85 | 44.68 | 45.03 |

Every step toward "lean wherever anything moves" scores better, up to a plain Catmull-Rom resample of the frame. That is the honest reading of the whole
line of work: without the game's sub-pixel camera jitter a temporal upscaler has no extra detail to find; what it still does is smooth (anti-alias)
what does not move, which a fidelity score cannot reward and the eye can. So the default stays where the owner's eyes put it ("look better",
2026-09-28) and *Lean from* moves it: lower for a picture closer to the game's own frame, higher for more of the upscaler's smoothing. Stability 0 to 0.9 on
FSR changed nothing on these clips (it is for thin lines).

## Sharpness at rest (2026-09-29, issue #8)

An upscaler user reported: sharper while moving, softer when standing still. That is the lean at work: moving, the picture is a resample of the frame
(sharper); at rest it is the upscaler's own picture. `nr_sreval still=1` holds one frame for 40 runs (a camera at rest), DLSS Auto at 1.5x:

| | Silent Hill f | World of Warcraft |
|---|---|---|
| the upscaler alone | 32.51 dB, detail 73 % | 32.53 dB, detail 58 % |
| 25 % plain resample kept (`restmix=25`) | 32.90, 74 % | 32.94, 59 % |
| 50 % | 33.23, 75 % | 33.31, 60 % |
| 100 % (a plain resample) | 33.56, 80 % | 33.74, 65 % |

(detail as a share of the game's own full-size frame; a plain stretch: 6.7 / 6.4 against the upscaler's 6.76 / 6.75 and the resample's 7.40 / 7.59.)
So at rest the temporal upscaler is softer than the plain resample it replaces: no jitter, nothing to accumulate. "Sharpness at rest" (setting
`scalerLeanRest`, `SrEngine::SetLeanRest`, the lean's floor) lets the person choose; default 0 (unchanged).

## An oracle for the motion (2026-09-29): accuracy is not what limits the upscalers

Question: is it the accuracy of our real-time motion that stops DLSS, FSR and XeSS from finding detail in Lossless Scaling's frames, or is there nothing to
find without the game's camera jitter? `nr_sreval oracle=1` gives them the motion between the recording's own full-size frames (`tools/oracle_flow.h`:
hierarchical 8x8 block matching, coarse to fine, sub-pixel at the end; the upscaler never sees those frames) instead of our estimate, with no lean.
Silent Hill f, the start of the clip, 2x from **point-sampled** input (what a game without anti-aliasing renders at a lower size: aliased, so a temporal
method has something to unfold), upscaled dB against the full-size frame (a plain stretch: 31.29):

| motion given | DLSS (L) | FSR 3.1 | XeSS |
|---|---|---|---|
| none | 30.87 | 26.93 | 25.26 |
| our estimate | 30.83 | | |
| **oracle** | 31.04 | 29.97 | 30.98 |
| oracle, sign flipped | 30.98 | 29.17 | 30.58 |

Motion matters a great deal to FSR and XeSS (none: 25 to 27 dB; right: 30 to 31), and our estimate is within 0.2 dB of the oracle for DLSS. But **not one of the
three gets above a plain stretch of the aliased input, even with (near-)perfect motion**. DLSS barely reads the history at all (the flipped sign changes
0.06 dB). Conclusion: **the direction "better motion so the temporal upscalers find real detail" is closed** for these engines on Lossless Scaling's frames: they have
nothing to accumulate (no jitter; the frames sit on the same pixel lattice) or reject what they get. What remains open is detail from somewhere else: a
neural single-image super-resolution model as the moving/at-rest picture (BACKLOG, "ideas for real detail", 2), or a game that does supply jitter (none through
Lossless Scaling). What the upscalers do give: anti-aliasing at rest and a steady picture; and, since 0.9.13, no trailing in motion.

## Steady sharpening: sharpening from a running average (2026-09-30)

The idea from the section above ("sharpening follows the temporal stability"), built as `Steady sharpening (test)` (`SrEngine::SetSteadySharpen`, `nr_sreval steady=N`).
Two passes replace the plain sharpening: the first keeps a running average of the picture in the SDR view (the previous average fetched along the measured
motion, clamped to this frame's 3x3 neighbourhood so a wrong motion cannot ghost, blended in by `steady`); the second sharpens that average and adds only
its sharpening to this frame. The game's shimmer averages out of the average, so it is not amplified.

First try, and why it was dropped: cutting the sharpening where the picture differs from the motion-compensated previous frame (a threshold on the difference)
scored the same as simply sharpening less (clip 23-54-43: flicker/detail 107 % / 128 % against plain 0.3's 108 % / 127 %); narrower bands moved along the same
curve by 1 to 3 points. The difference cannot tell shimmer from real edges the block motion misses.

The running average, DLSS model E, 1:1, `sharpen=70` (the owner's), 40 frames of each clip (flicker / detail against the game's own frames):

| clip | plain 0 | plain 0.15 | plain 0.3 | plain 0.7 | steady 0.8, sharpen 0.7 |
|---|---|---|---|---|---|
| 23-54-01 (nearly still) | 93 / 86 | 111 / 125 | 114 / 130 | 125 / 151 | **105 / 129** (sharpen 1.0, steady 0.9: 111 / 151) |
| 23-54-20 | 90 / 85 | 105 / 127 | 107 / 132 | 117 / 156 | 100 / 118 |
| 23-54-34 | 98 / 92 | 106 / 143 | 107 / 149 | 112 / 178 | 103 / 131 |
| 23-54-47 | 89 / 83 | 109 / 126 | 112 / 132 | 125 / 159 | 102 / 116 |

At equal detail the running average has about 9 to 14 points less flicker on the nearly still clip and 1 to 2 points less on the three moving ones (read off
the line between the plain points); it was never worse. The history is fetched with the motion's sign reversed (`steadysign=1` for the other way; clip 23-54-43 at steady 0.5: 112 % / 134 %
against 107 % / 126 % reversed). Upscaling 1.5x (23-54-01, `shrink=150`): 33.75 dB against 33.71, and the steadiness 47.5 against
46.9 dB. Cost: two more full-size passes and one copy. Not tried live (the metrics are the game's own frames, not the owner's eyes); off by default.

## Choosing the defaults for upscaling (2026-09-30)

Most Lossless Scaling use is upscaling (about 1.5x), where the recordings give the true picture to score against. `tools/sr_default_grid.py 150 dlss <3 clips>`
(WoW 23-54-01 and 23-54-20, Silent Hill f 17-54-39; frames 10 to 33, DLSS Auto model; each cell upscaled dB / steady dB, the mean over the clips):

| Sharpening | Sharpness at rest 0 | 0.3 | 0.6 | (Steady sharpening 0.6 at rest 0.6) |
|---|---|---|---|---|
| 0.3 | 36.66 / 39.13 | 37.32 / 39.78 | 37.97 / 40.51 | 37.56 / 39.77 |
| 0.5 | 36.49 / 38.91 | 37.17 / 39.56 | 37.83 / 40.28 | 37.41 / 39.56 |
| 0.7 | 36.09 / 38.36 | 36.79 / 39.00 | 37.44 / 39.67 | 37.07 / 39.07 |

- **Sharpness at rest** improves both scores at every step: +1.3 dB and +1.4 dB steady at 0.6 against 0. New default **0.5** (was 0).
- Sharpening costs the score (sharpening always does against a true frame: 0.5 is 0.14 dB below 0.3 at rest 0.6); it stays at 0.5 (NIS parity, chosen by eye).
- **Steady sharpening** is neutral to slightly negative when upscaling (-0.3 to -0.4 dB on average, -0.8 on the Silent Hill f clip, which moves); its measured benefit
  is at 1:1 (previous section). It stays off by default.
- `tools/sr_option_check.sh 150 10 24 <clips>` (sharpen 0.5, rest 0.5): **EASU** for the lean ("Crisp edges") is worse on the scores, -0.15 dB on the WoW clips and
  -1.75 dB (and 2 dB less steady) on Silent Hill f; it looks crisper, so it stays an opt-in test. **Motion by shape** changes nothing (under 0.03 dB either way):
  off, and not worth its 0.5 s of shader compile.

## Steady sharpening trusts its average less in motion, and is one pass (2026-09-30)

At 1.5x the running average lost up to 0.96 dB against the true picture on the moving Silent Hill f clip (the section above). The average's weight now falls with the
pixel's motion: full below 0.5 output pixels a frame, gone at 3 (`steadymv=A steadymv2=B`, tenths of a pixel, in `nr_sreval`). Upscaling 1.5x, sharpen 0.5, rest 0.5,
steady 0.6 (upscaled dB / steady dB), off against on:

| clip | steady 0 | steady 0.6, no band | steady 0.6, band 0.5 to 3 px |
|---|---|---|---|
| 23-54-01 | 35.34 / 41.83 | 35.31 / 41.54 | 35.35 / 41.78 |
| 23-54-20 | 34.44 / 37.83 | 34.26 / 37.19 | 34.37 / 37.59 |
| shf 17-54-39 | 43.07 / 40.50 | 42.11 / 39.38 | 43.01 / 40.43 |

So with the band the option is within 0.1 dB of off when upscaling. At 1:1 (sharpen 0.7, steady 0.8, flicker / detail against the game's own frames) the band keeps the gain on
a nearly still scene (23-54-01: plain 120 % / 150 %, no band 102 % / 125 %, band 112 % / 145 %: about 10 points less flicker at equal detail) and gives up the 1 to 2 points
in motion (23-54-20: plain 117 % / 156 %, band 114 % / 151 %; 23-54-47: 125 % / 159 % against 122 % / 153 %), which is the honest size of the gain there.

It is also one pass now: one 8x8 group loads a 12x12 tile once, works the average out for the 10x10 around its pixels, sharpens from that and writes the average for the next
frame into the second of two history textures (no temporary picture, no copy). The offline tool's GPU times are noisy (the GPU idles between frames), but the pass after the
upscaler went from about +3 ms (two passes and a copy) to about +1.8 ms over plain sharpening at 4K 1:1; live times are in the log ("after the upscaler").

## FSR's sharpening, and Steady sharpening on XeSS and FSR (2026-09-30)

Upscaling 1.5x, sharpen 0.5, rest 0.5, three clips (WoW 23-54-01, 23-54-20, Silent Hill f 17-54-39; upscaled dB / steady dB):

| | wowb 01 | wowb 20 | shf |
|---|---|---|---|
| XeSS, steady 0 | 35.47 / 42.11 | 34.69 / 38.23 | 43.40 / 40.79 |
| XeSS, steady 0.6 | 35.47 / 42.00 | 34.64 / 37.97 | 43.32 / 40.69 |
| FSR with AMD's RCAS (identical with steady 0.6: RCAS sharpens, our pass does not run below strength 1) | 34.61 / 42.10 | 33.69 / 37.26 | 43.37 / 40.42 |
| FSR with our pass, steady 0 | 35.14 / 42.06 | 34.30 / 37.96 | 43.39 / 40.78 |
| FSR with our pass, steady 0.6 | 35.16 / 41.96 | 34.25 / 37.70 | 43.31 / 40.69 |

No-reference detail against the game's own frames (wowb 01 | shf): FSR with RCAS at 0.5 keeps 73 % | 83 % of the game's detail (1.0: 78 % | 84 %); with our pass 0.2: 96 % | 105 %,
0.3: 98 % | 107 %, 0.5: 101 % | 112 %. So RCAS at the default is much softer than DLSS and XeSS at the same slider; FSR now uses our pass (`fsrown=0` in `nr_sreval` for the old way).

## What the passes after the upscaler cost (2026-09-30)

`nr_sreval bench=300` runs the last frame 300 times back to back, so the GPU stays at full clocks (the per-frame times printed without it are 3 to 4 times too high). 4K out,
1.5x, LDR, RTX 4070 Ti SUPER, model E: the whole run 2.13 ms with no passes after the upscaler; plain sharpening (0.5) +0.25 ms, the lean (Catmull-Rom or EASU alike) +0.32 ms,
both +0.55 ms, both with Steady sharpening +0.74 ms (+0.17 for it). In HDR (fp16, 66 MB a picture) one live log had 0.97 ms for the lean and the two-pass steady sharpening: six full pictures
read or written is about 0.5 GB, about 1 ms at the card's 500 GB/s, so bytes, not arithmetic, are the limit there. Folding the lean into the sharpening tile was rejected: it would evaluate
the lean 2.25 times for the halo, and the lean is already about twice its own bandwidth time. Done: the running average in R10G10B10A2 (4 bytes), and no history read where it is not trusted.

## Sharing the motion between generated frames (2026-09-30)

With frame generation the upscaler is given two frames for each the game draws; the log says "2 presented per real frame" and the motion estimate took 0.66 ms of a presented
frame's 2.47 ms. `SrEngine::SetFlowReuse` / `nr_sreval flowreuse=N` keep the last estimate on every other frame: 1 keeps the block vectors and only refines them per pixel,
2 keeps the vectors and the distrust mask untouched (the pyramid, which the next estimate needs, is still built). Upscaling 1.5x, sharpen 0.5, rest 0.5, steady 0.6, the
recordings' frames as consecutive steps (whole game frames, so a harsher test than half steps; upscaled dB / steady dB):

| | wowb 01 | wowb 20 | shf | motion estimate, GPU (`bench=400`) |
|---|---|---|---|---|
| every frame | 35.35 / 41.79 | 34.37 / 37.59 | 43.01 / 40.43 | 0.39 ms |
| 1: refine only | 35.30 / 41.58 | 34.34 / 37.45 | 42.31 / 39.55 | 0.28 ms |
| 2: keep everything | 35.34 / 41.79 | 34.37 / 37.56 | 43.07 / 40.44 | 0.23 ms |

Mode 1 loses (a stale block vector misleads the per-pixel step); mode 2 is as good as estimating every frame and 41 % cheaper for the estimate. The whole run's time did not move in the
back-to-back benchmark (it is limited by the CPU side or the upscaler there), so the gain is the estimate's own 0.16 ms a frame, about 6 % of a presented frame here; live it is
"motion" in the log ("DLSS 2.47 ms a presented frame (motion 0.66)" before). The panel has "Share motion between generated frames" (on); it acts only when two or more frames are presented per real one.

## Where the upscalers add flicker: the Durotar spot, and sharpening less in motion (2026-09-30)

The owner's recording of Durotar (red cracked ground, walking; 2562x1442 HDR frames as Lossless Scaling captured them, 34 frames a second) at 1:1 (`nr_sreval shrink=100`, model E, flicker and detail
against the game's own frames): no sharpening keeps 95 % of the flicker and 95 % of the detail; the owner's sharpening 0.45 with Sharpness at rest 0.6 gives **113-115 % and 136-141 %** for DLSS, FSR and
XeSS alike (113 %, 133 % for XeSS). Steady sharpening adds little here (115 -> 113 %): the ground moves faster than its trust band, and widening the band (1-6, 2-10, 4-20 pixels a frame) only slides
along the plain curve: a running average cannot help in fast motion. What the eye does not resolve in fast motion is the added detail, so the sharpening is cut there instead
(`SetMoveCut`, `nr_sreval movecut=N`, smoothly from 1 to 8 output pixels a frame): flicker / detail 113 % / 137 % at 0, 110 / 131 at 0.3, 107 / 127 at 0.5, 105 / 123 at 0.7, 102 / 117 at 1.0. The plain
curve (sharpening less everywhere) costs about 0.43 flicker points per detail point, so 102 % would cost it 26 detail points (111 %); the cut keeps 117 %. Upscaling 1.5x (upscaled dB / steady dB, wowb 01,
wowb 20, shf): cut 0 35.35/41.79, 34.37/37.59, 43.01/40.43; cut 0.5 35.39/42.09, 34.40/37.68, 43.82/41.21; cut 1.0 35.36/41.69, 34.34/37.39, 43.18/40.30. The pass time is unchanged (0.71-0.73 ms).

## HDR in fp16, and the highlight specks (2026-09-30)

`nr_sreval hdr=1` keeps an HDR recording's frames as fp16 light (1 = the SDR white; the recording's own scRGB taken at SDR white 200 nits, as `nr_lsrec export` and the screenshots take it) through the
engine, scores them tone-mapped, and checks the light: finite, mean, peak. On the Durotar clip (HDR, 2562x1442, upscaled 1.5x, DLSS model E): no passes after the upscaler: peak light 1.28 for an input peak of 1.20, mean 0.998;
with plain sharpening (0.45): **peak 125.75**, mean 1.026; with everything (sharpening 0.45, Sharpness at rest 0.6, Steady sharpening 0.6, Sharpen less in fast motion 0.5, EASU): peak 125.75. The roll-off
(`hdr_hlsl.h`: identity to 0.75, then logarithmic, 1.0 = 125.75 times the SDR white) makes a view of 1.0 a 10,000-nit highlight, and `c.rgb + (SdrToLight(r) - SdrToLight(v))` with `r` saturated at 1.0 lets a change that
only nudges a bright pixel to 1.0 do that (0.02 of the view near the top is three times the light). Fixed by `ApplyViewChange` (the result within 1.5x + 0.5 of the light it had) and `WithinNeighbours` (within 15 % of the range of
the neighbours' light). After: peak 1.44, mean 1.003, scores within 0.1 dB. On a made-up clip of 1.9-light glints (`nr_lsrec make ... hdr=1`): FSR alone 1.86, XeSS alone 2.24 (the upscalers overshoot their own glints), plain sharpening
3.1 and 3.8 before the limit, 2.2 and 2.6 after; everything on 2.80 (Catmull-Rom's lean alone 2.39, EASU's 1.90). Also run with the D3D12 debug layer: no messages.

## The upscaler on the real frames only, with frame generation (2026-10-01)

The owner's HDR log at 1.5x, DLSS preset E, frame generation x2: the DLSS side costs 3.3 ms of GPU for every presented frame (the upscaler 2.2, the passes after it about 1, the motion 0.27), so 6.6 ms for each real frame
of 28 ms. The question: run DLSS on the real frames only, and give the generated ones a cheap picture. `SrEngine::SetCheapNext` is that run: no motion estimate, no upscaler; the lean takes the last upscaled picture, moved
along the last real frame's motion by half a step (`SetPresentStep`, the lean's `warp`), toward this frame's plain stretch (Catmull-Rom) where the two disagree, then the sharpening as usual (about 0.6 ms of the 3.3).
`nr_sreval realonly=1` emulates it on a recording taken as the presented frames (the odd frames are the generated ones; the engine is given none of them); `realonly=2` is the plain stretch on the CPU instead; `step=`, `warpsign=` as tests.
DLSS, 1.5x, sharpening 0.5, 32 frames, all frames upscaled (A) against real frames only (B):

| clip | A: dB / steady dB / flicker / detail | B: dB / steady dB / flicker / detail |
|---|---|---|
| SDR, Silent Hill f, fast turns (1) | 38.40 / 35.80 / 101 % / 105 % | 37.64 / 34.91 / 100 % / 107 % |
| SDR, Silent Hill f (2) | 36.54 / 33.87 / 100 % / 99 % | 36.11 / 33.37 / 100 % / 100 % |
| HDR, WoW hall (1) | 25.96 / 42.23 / 100 % / 106 % | 25.97 / 39.23 / 110 % / 106 % |
| HDR, WoW hall (2) | 29.15 / 40.49 / 96 % / 112 % | 29.22 / 39.30 / 110 % / 112 % |

What it took to get there (each tried on all four clips): the plain stretch alone for the generated frames scores as well as DLSS in the noisy SDR clips (+0.2 dB) but in the calm HDR hall DLSS and a plain stretch differ
by about three levels of 255, so alternating them flickered at 222 to 321 % of the game's own; keeping the last upscaled picture as it was (no moving) fixed the flicker (92 to 98 %) and lost 0.3 to 0.55 dB in motion; moving it half a step
along the motion helped only with the motion sign the history fetch does not use (`warpsign=1`); an agreement test between the upscaled picture and this frame, compared at the scale of a few pixels (the detail differs by design), sends
the pixels that moved or were uncovered to the plain stretch (a test on single pixels flickered, one that is too loose lost 4 dB in a fast turn). The numbers above are the best balance found: **about equal in detail and flicker, 0.4 to 0.8 dB
lower in fast SDR motion, 1 to 3 dB lower on the steadiness score in the HDR hall**: a performance mode, not a free one. At 4K 1.5x it would take the DLSS side from 3.3 to about 2 ms of GPU per presented frame (the generated ones
cost about 0.7), 2.6 ms less for each real frame at x2, near 10 % of a 28 ms frame.

Not wired live. It needs (1) to know which presented picture is the real one (the capture pass sees the real frame; LS presents the generated frame before or after it, to be found out) and to carry a cheap flag with each queued run to the
engine's thread (`SetCheapNext` is a flag for the next run, enough for the offline loop); (2) the estimator to be left to the real frames and `SetPresentStep` set to the time ratio (runtime.cpp `PresentStepFraction`);
(3) the owner's eyes on a calm scene and a fast one, which no number replaces. An option "Lighter upscaling with frame generation", off by default, once it has been seen live.
