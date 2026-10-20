# EchoFG: a frame generator of our own for Lossless Scaling: what we know, what the lab says, the plan (2026-10-09)

**Name (the owner's decision, 2026-10-09): EchoFG.** The panel, the log and the notes call the feature that; `OwnSynth` and "our own synthesis" stay as the technical names of the method in the code.

Goal (the owner's, 2026-10-09): a generator that **works with Lossless Scaling** (it sits in the same slot as LSFG, on LS's frames) and that we can improve ourselves, built on our own shaders.

## The boundary

- **Not** a modified or re-shipped copy of Lossless Scaling's frame generation (its shaders and binary are the author's paid, closed work). **Not** a reading of its shader bytecode to copy the algorithm: the shaders are inside Lossless.dll, and
  taking them apart to reproduce them is reverse engineering a commercial product for its secrets; we do not do that.
- **Yes**: our own algorithms from public research, written in our own shaders; what lsfg-vk's open host code shows about the *shape* of such a pipeline (counts, sizes, what feeds what), as ideas; LSFG as a black box to compare against by eye.
- lsfg-vk: the code we read is the GPL-3.0-era archive (`git.lsfg-vk.dev/lsfg-vk-archive`; the project has since gone to CC BY-NC-ND 4.0, see docs/lsfg-vk-study.md). Either way: read and learn, copy nothing.

## What lsfg-vk's host code shows about an LSFG-class pipeline (structure only; the math is in LS's shaders and not here)

Read 2026-10-09 (`lsfgvk.cpp`, `helpers/utils.cpp`, the `shaderchains/*.cpp` files):

- **Two kinds of work.** *Per real frame*: a luma mip chain (7 levels, R8), then at each of the 7 levels a 3-pass **feature** chain (full -> half -> half -> quarter size; `2m` feature images at quarter size, m = 1 in performance mode, 2 in quality mode), then a 6-image
  **second chain** (`beta`, one per level) that the main pass reads. *Per generated frame*: a **7-level coarse-to-fine chain** (`gamma`, each level reading its own features, the coarser level's result and that level's `beta`), then at the **three finest levels only** two **refinement chains** of
  four passes each (`delta`: two outputs, RGBA16F, full size), then the **synthesis** pass that reads the two real frames (swapped in order on alternate frames) and three of those results and writes the frame.
- **The time is a constant, not a structure.** Each generated frame has a small constant buffer: `resolutionInvScale` (1 / flow scale), `timestamp = (i + 1) / (N + 1)` for generated frame i of N between two real frames, and **`uiThreshold = 0.5`**.
  So a multiplier is only more generated frames with different timestamps; the features are shared by all of them, the matching is done again for each.
- **Flow scale** is the motion estimation's size relative to the picture (`flowExtent = size / flow`): the whole matching chain runs at the reduced size, which is how LSFG stays cheap at 4K.
- **A UI threshold** is a parameter of the generator itself: static interface elements (the frames agree there) are handled by a rule with a threshold, not left to the motion search.
- HDR is a format choice (RGBA16F frames in place of RGBA8) with its own synthesis shader; a half-precision variant of every shader exists.

## What the CPU lab says (`tools/interp_lab.py`, numpy only, 3 clips, frames 5x smaller, rebuilding the middle frame of three)

Method scores, mean PSNR of the rebuilt frame against the real one:

| clip | blend | fwd (block motion, move both frames half way) | bilateral (symmetric search at the middle frame), coarse to fine | + fallback to blend where the match is poor |
|---|---|---|---|---|
| Silent Hill f, fast motion | 25.4 | 26.1 | 25.7 | 25.8 |
| WoW, mostly still | 36.5 | 36.1 | 35.9 | 36.1 |

- **Plain blending is a hard floor to beat in PSNR**: it hedges. A method that commits to a motion is punished heavily where it is wrong (a few wrong pixels cost more than a halved contrast everywhere), so in a still scene any method that moves things loses to the blend (single-scale search: 36 vs 49 dB).
- **A single-scale search is the wrong tool**: on fast motion it saturated its range; the coarse-to-fine version reaches far enough and no longer damages still scenes.
- **Even then, simple block matching gains only 0.3 to 0.7 dB on the fast clip.** FSR 3.1 frame generation with our motion estimate scored **30.4 dB against 26.1 for the blend** on the same kind of clips (docs/frame-generation-research.md). So what makes the gap is *not* "compensate the motion" but
  the quality of the motion (sub-block, edge-aware, confident) and above all the **synthesis around it: occlusion and disocclusion** (what is covered, what is uncovered), which is where hedging loses and committing wins.
- **Headroom**: an oracle that picks, per pixel, the best of (frame a, frame c, blend, motion-compensated) with the truth in hand reaches +4.7 dB over the blend on the fast clip with these weak candidates; a good selection rule is worth a lot.

## Stage 1 result: synthesis on our own flow (2026-10-09, `tools/interp_flow_lab.py`)

`nr_fgeval ... flowdump=N` writes raw triples (frame before, the real frame between, this frame, this project's per-pixel vectors, the time of the frame between); the lab rebuilds the middle frame with each method and scores it. 3 Silent Hill f clips, 10 triples each, frames 4x smaller (so the dB
are not comparable to the full-size FSR numbers, only to each other). Vectors: the estimator's, the same that feed FSR frame generation.

| method | fast clip (s1) | calm (s2) | calm (s3) |
|---|---|---|---|
| blend | 21.2 | 31.0 | 28.8 |
| half: gather both frames half way along the vector read at the output pixel | 26.3 | 39.9 | 37.0 |
| **spl: vectors first splatted to where their pixels are at the frame's time** (best match wins a spot, holes filled from neighbours) | **27.1** | **40.2** | **37.7** |
| oracle (best candidate per pixel, truth known; not a method) | 32.4 | 43.6 | 41.5 |

What was learnt:
- **Good vectors do the work: +5 to +9 dB over a blend with the plain half-way gather** (the same size of lead FSR frame generation shows). The synthesis on top is worth about **+0.3 to +0.9 dB, consistently, from splatting** the vectors to the frame's time so that each output pixel reads a vector that belongs to it.
- **Catmull-Rom sampling in place of bilinear: +0.4 dB on the calm clip, nothing on the fast one.** Small; cheap in a shader.
- **Rules that fell back to the unmoved mean or took one side where the moved pair disagrees all LOST** (-0.2 to -3.5 dB): a "still" test (the two frames agree without motion) is wrong in a camera pan, where flat areas agree unmoved too; the occlusion pick (holes take a, poor matches take c) took the wrong side more often than the right one. Per-channel medians of the candidates lost too (-0.3 to -0.8). So **blind rules are not the way to the oracle's 4 to 5 dB**; they need the right information (see below).
- **Vector selection among neighbours and per-pixel nudging (+-1 px) at the output pixel: +0.0.** The estimator's vectors are already as good as local search on pair agreement can make them.
- **Uneven pacing is real in recordings**: in 3 of 10 triples of the fast clip the real middle frame shows the picture at t = 0.3, not 0.5 (the capture clock says 0.5; the game itself advanced unevenly). Those triples looked like generator failures (blend and half-way both scored 21 to 22 dB) and scored 25 dB at t = 0.3. Scores should be read with that in mind (`--auto-t` picks the best t per triple for every method alike), and a real generator should not expect "half way" to be the truth.
- A **UI/HUD rule** could not be tested: these clips have none. It needs a clip with a static HUD over moving background (WoW, or a game with a minimap); it must be a *local, relative* test, not "frames agree".

### Stage 2 result: both flows (same day)

`flowdump` now also writes the vectors the other way (the estimator run on the pair swapped; note it disturbs `nr_fgeval`'s own FSR score in that run, so use the dump run for the dump only). Method **bi**: the vectors of both directions, each splatted to the frame's time, used only where the flow the other way agrees
(forward-backward consistency; the threshold hardly matters), per output pixel the hypothesis whose moved pair agrees better; where neither reached, `spl`.

| | fast clip | calm s2 | calm s3 |
|---|---|---|---|
| half | 26.28 | 39.88 | 37.03 |
| spl | 27.14 | 40.17 | 37.66 |
| **bi** | **27.60** | **40.28** | **37.92** |
| oracle | 32.83 | 43.68 | 41.57 |

- **bi beats spl on every triple, by +0.1 to +0.5 dB (most on the fast clip); against the plain half-way gather it is +0.4 to +1.3 dB.** Real, small, and it costs a second flow estimate (about the cost of the first).
- On the fast clip 37% of output pixels are reached by neither flow, and the consistency check marks only 59% of vectors good: that is where the remaining error is. The oracle gap (4 to 5 dB) is **not** closed by local rules on this information; what is left is mostly hole and occluded areas and sub-pixel precision on texture.
- This is a level with FSR frame generation (+4 dB over blend on these clips) and small improvements on top, not a different league; whether it looks better than LSFG is a live judgement.

### Stage 3 result: direction-aware hole fill, and the HUD question (same day)

- **bi+dir: a pixel neither flow reached takes the vector of the nearest reached pixel to its left, right, above or below (or none), whichever makes the moved pair agree best** (a hole is mostly background just uncovered, so one side's vector is right and the average of both sides is not).
  Fast clip 27.60 -> **28.16**, calm 40.28 -> 40.33 and 37.92 -> 38.06; two WoW 4K clips (with a HUD) 37.34 -> 37.58 and 35.05 -> 35.42. **Every triple of every clip is better than without.** Applying the same candidates to *every* pixel (own vector kept unless another agrees clearly better) added nothing.
  Cumulative against the plain half-way gather: +1.9 dB (fast), +0.5, +1.0 (calm), about +2 (WoW); against a blend +6.9, +9.4, +9.3, about +6.
- **Static interface: no rule needed offline.** On pixels that are truly static (frames a, c and the truth agree within 4 levels over 5x5: the WoW HUD, 15-18% of those frames) every moved method has the same error as the blend (the estimator already gives a HUD that stays put a zero vector), and those pixels carry 0.1% of the total error. What stays unmeasured is the HUD *border* over moving background, a live-eye question.
- **A flow defined at the frame's own time (idea 2) was not built**: nudging the splatted vector per pixel to the best local agreement (`ref`) gained +0.0, so a flow re-estimated at t would start from the same ceiling. Skipped for now.
- **FSR on the 4K WoW clips scores *below* a blend in `nr_fgeval`** (23 against 29 dB), but those runs had the second estimator active (see above), so the number is not to be trusted; rerun without `flowdump` before drawing anything from it.

- **A soft confidence (blend toward the unmoved mean as far as the moved pair disagrees, up to 50-80%) gained +0.0 to +0.07 dB**: hedging does not close the oracle gap, and would ghost to the eye. Dropped.

Remaining ideas, in order of likely value: (1) the lab is at diminishing returns on cheap local rules (the gap to the oracle is texture and genuinely hidden content); the next real gains are a better flow itself or a learned method, both bigger jobs; (2) the HUD border over moving background, judged live; (3) a second, finer estimate in the holes only (they are 37% of the fast clip); (4) then shaders in `FgEngine` (stage 5 below), behind the existing off switch.

### Stage 5 result: the shaders (same day): `src/engine/own_synth.{h,cpp}`

The lab's method as compute passes in our own shaders, with no part of FSR: match error (5x5 mean), the vectors splatted to the frame's time (an atomic minimum on error, then the winner writes its vector), the reversed vectors made by splatting onto a's pixels (no second estimate: the lab's `--invert` kept 70-95% of the gain of a real second flow),
those splatted to the time too, the hypothesis that agrees better per pixel, holes from the nearest reached pixel in four directions, the frame. `nr_fgeval own=1` (offline, kept frames) and `engine=1 engineown=1` (through `FgEngine`, as live) score it; the setting "Make the frames with our own synthesis instead of FSR (test)" (frame generation prototype, off by default) switches it live; FSR remains the fallback (a frame format without typed stores).

| clip (2560x1440, 20 rebuilt frames) | FSR 3.1 + our motion | our synthesis (offline) | our synthesis (through FgEngine) | blend |
|---|---|---|---|---|
| fast | 24.61 | 24.40 | 24.40 | 20.50 |
| calm 1 | 34.12 | 34.07 | 34.08 | 28.84 |
| calm 2 | 31.09 | 31.05 | | 27.03 |

- **Level with FSR (within 0.05 to 0.2 dB) on its own code, and no AMD runtime needed for it.** Looking at the pictures: the character (head, collar, emblem) intact, no fragments at the edge, where FSR's known weakness is; a little speckle in foliage at edges.
- **Cost, unoptimised: about 2.5 to 3.7 ms of GPU per generated frame at 2560x1440** for the synthesis passes (the timing is noisy while the card is shared), on top of the motion estimate. Per pass (`NR_SYNTH_STOP=N` runs the first N): errors and splats ~1.1 ms, hypothesis passes ~1 ms, the final pass 0.3 ms. Obvious savings: run the splat and hypothesis passes at half size, one bigger pass per pair of splats. **(Correction 2026-10-20: these and the other per-stage times in this file were taken on a card idling at 435 MHz and are several times too high; at steady clocks the synthesis is 0.7 ms at 2560x1440. See docs/mfg-super-motion-engine-notes.md, "The synthesis cost measured at steady clocks".)**
- **On the two 4K WoW clips (3840x2160, a HUD, 20 rebuilt frames each) the same comparison, FSR 3.1 proper (no dump option running) against ours: 32.01 / 31.66 dB against 32.09 / 31.70** (blend 28.85 / 28.82). Level on all five clips, a little ahead on those two.
- **Not done**: HDR (scRGB / PQ frames are sampled as they are, which is only right-ish), the time t is fixed at 0.5 (a multiplier needs the timestamps (i+1)/(N+1): every pass but the splat and the last is per-frame-count anyway), pacing, a real-game live look.
- **Two bugs found by running it through the engine**: FSR's own dispatch still ran after ours in the same list (overwrote the frame, and failed for lack of its prepare step); and the buffers are shared by frames in flight (a UAV barrier first in `Record` orders them).

### Stage 6 result: half-size analysis and multipliers (same day)

- **The analysis (errors, splats, hypotheses, holes) runs on 2x2-pixel cells (half size) and only the last pass runs at full size**, where each pixel takes, of the 2x2 nearest cells' vectors, the one under which the moved pair agrees best at that pixel. Scores, full-size analysis (`ownscale=1`) against half size (`ownscale=2`, now the default): 24.40 -> **24.46**, 34.08 -> **34.10**, 31.05 -> **31.11** dB: **half size is better, not just cheaper** (a smoother field, a larger window), with a quarter of the buffers (11 x 4 bytes a cell: 165 MB at 2560x1440 full size, about 40 MB now) and about a third of the time when the card is quiet.
- **Multipliers: `OwnSynth::Record` takes a list of targets (time t each, up to 8: x9).** The errors and the vectors onto a's cells are made once for the pair; every frame made adds the splats, the hypotheses, the holes and the final pass. `FgEngine::GenerateFrames` makes up to that many per real frame (FSR still makes the one at half way), `framegen11` shows them evenly spaced (frame k of M at lead + k T / (M + 1), the real frame at lead + M T / (M + 1); M = 1 is the old timing) and the setting "Frames for each real frame" (x2 to x9, with our own synthesis only) sets it. Offline: the frame at t = 0.5 scores the same (31.11 dB) for x2, x4, x8, through the synthesis alone and through the engine (`enginemult=4`); frames at t = 0.25 / 0.75 against the middle truth score 27.6 / 26.6 dB, between the hold (25.6) and the middle frame (31.1), as they should.
- **Cost (GPU, 2560x1440, best frames, a quiet card): about 1.4 ms for x2, then about 1.2 ms for each frame more** (x4 about 4 ms, x8 about 9 ms; medians are 1.5 to 2x the best when anything else uses the card). At 4K about 2.25x. Per frame: errors and onto-a splat once (about 0.2 ms), then the splats 0.1, the hypotheses about 0.4, holes 0.15, the final pass 0.4.
- **The presenter is tested on a real swap chain** (`nr_fgpresent`, the addon's Present hook, a moving square): x2, x3, x4, x8, HDR (scRGB), FSR (x2), a 60 fps feed with every frame twice, the multiplier changed while running, a resize of the swap chain: the right number of frames per real one, in order, the square exactly where it should be (0.00 px error SDR, 0.2 px HDR), presents evenly spaced after the first interval. **Not tested**: inside Lossless Scaling itself (its own presenting thread, its swap chain handling, other games' formats).

### An independent review of this code (same day) and what it changed

A reviewer who had not seen the work read the shaders, `FgEngine` and the presenter and reported twelve findings; all but two were real and are fixed:
- **At x3 and up the presenter held Lossless Scaling's thread longer than one real-frame interval** (the real frame is let go at lead + M/(M+1) of it; the lead was capped at half an interval), so the real rate sagged. The lead is now capped at one frame's share of the interval (`T / (M + 1) - 1 ms`).
- **A stale fence event could let a wait through** after a timed-out wait (an auto-reset event shared by every wait), so a command allocator could be reset while the GPU still used it. The wait now re-asks the fence until it has really reached the value; the list resets are checked.
- **Frame generation switched off left the engine, the D3D12 device and about 100 MB of buffers and shared textures resident** (nothing called `framegen::Shutdown`): it is torn down on the switch and at the addon's shutdown.
- **The 5x5 mean of a's errors counted the "not reached" marker**, so a's hypothesis competed on how many unreached neighbours it had; it skips them now.
- **A gap in the real frames (a pause, a double present) did not reset the engine**: one frame was made across the gap. **FSR after our own frames** (the setting switched off) now starts afresh. **The made-fence is always waited for** on the D3D11 side, so the next copy into the shared input cannot overtake the engine's reading of the last. The setters no longer take the presenter's lock (which it holds through its waits) and the panel reads a published copy of the statistics.
- **Blending in the SDR view clipped wide-gamut and negative scRGB colours** in the made frames: the choice of where to look still uses the view, the blend is on the frames' own values now. `Ensure` leaves nothing half made after a failed allocation; the splat key mixes the index's high bits; a `static_assert` ties the slot counts together.
- Two findings were suspicions that needed a runtime check: **teardown of the static presenter state at DLL unload under the loader lock** (now mostly moot: the shutdown happens before) and **the hitch when EchoFG is switched on** (see the next paragraph).

**Off the presenting thread (same day, after the review).** Switching frame generation on used to hold Lossless Scaling's presenting thread for about 3 seconds (AMD's runtime, the motion estimate's six shaders, EchoFG's nine pipelines, the shared frames): the engine and EchoFG now start on a thread of their own, the shared fences and frames are made there too, and until it is ready the frames between are the plain blend (the real frames go on at their rate). `nr_fgpresent` measures it: the longest wait between the presenting thread's calls into the presenter is one real-frame interval, also across the start (about 1.3 s for FSR's frames, 3.4 s with EchoFG), a resize (the engine restarts at the new size, blend meanwhile) and switching EchoFG on mid-run (FSR makes the frames for about 2 s, then EchoFG). One 50 ms hitch remains in the engine's very first frame.
After the fixes: the three clips score 24.41 / 34.10 / 31.11 dB (24.46 / 34.10 / 31.11 before: the sentinel change cost 0.05 dB on the fast clip), HDR 34.09, and every `nr_fgpresent` scenario passes (x2 to x8, HDR, FSR, dup, switch, resize).

### A third, older frame (same day): does not pay

The idea: the area just uncovered in the frame between may be visible in the frame before the frame before (a0), and the pair before's motion may say where the holes' content goes. `nr_fgeval flowdump` writes a0 and the motion from a to it (FLT3 files from the third kept frame on); `interp_flow_lab.py --third` tests it on 9 to 10 triples of each of three clips.

| (mean of the clip) | fast s1 | calm s2 | calm s3 |
|---|---|---|---|
| bi+dir (the method as it is) | 28.15 | 40.28 | 38.10 |
| holes also try the motion carried on from the pair before | 28.08 | 40.27 | 38.06 |
| that motion tried at every pixel, replacing ours where the moved pair agrees clearly better | 28.06 | 40.28 | 38.08 |
| oracle (best candidate per pixel, truth known) | 32.79 | 43.65 | 41.67 |
| ... plus a *control* candidate (the same geometry sampled from a, a frame we have) | 33.32 | 43.94 | 41.97 |
| ... plus a0 along the same vector | 33.76 | 44.07 | 42.14 |

- **The motion prior from the pair before changes nothing** (it wins at 0.2 to 1 % of the pixels, and loses a hair of score: -0.03 to -0.07 dB).
- **The older frame's picture is worth little even with a perfect choice**: an extra candidate raises a per-pixel oracle by itself (the control: +0.3 to +0.5 dB); a0 adds only 0.13 to 0.44 dB beyond that, in the *oracle*; a real selection rule would get a fraction of it. Against a third frame's cost (another frame kept, another copy a frame, the estimator's older motion kept too) it does not pay.
- **Verdict: not built.** The remaining gap to the oracle is not content hidden in the two frames we have and visible in an older one; it is fine texture and hidden content in both. A better flow or a learned method are what is left.
### Fast motion at a low frame rate (2026-10-11, after the owner's first live test: "fast motion really bad, tearing and glitches")

The owner's Saints Row 2 test (a native 30 fps game, 3840x2160 HDR, EchoFG x2) had torn-looking frames in fast camera motion. `nr_fgeval step=N` keeps every Nth recorded frame, so the motion between the kept frames is N times as big (a fast turn at 30 fps). Mean score of the rebuilt frames (dB), same clips:

| clip | motion | FSR 3.1 | EchoFG | blend |
|---|---|---|---|---|
| fast s1 | x1 | 25.29 | 25.19 | 21.70 |
| | x2 | 21.69 | 21.69 | 19.79 |
| | x3 | 20.08 | 20.21 | 19.10 |
| | x4 | 17.70 | 19.18 | 18.37 |
| calm s3 | x1 | 30.21 | 30.25 | 26.55 |
| | x4 | 24.85 | 25.16 | 24.65 |

- **At three to four times the motion of my fastest clips both generators are no better than a plain blend**, and FSR falls below it: that regime (180 px between frames at 2560 wide, 7 % of the width) is a fast turn at 30 fps. The picture (`docs/images/echofg-vs-fsr-4x-motion.jpg`: the real frame, EchoFG, FSR 3.1, the same frame) shows both breaking, FSR worse (the character smeared and torn), EchoFG with blocky patches on the periodic wooden wall, flecks in the hair and fragmented leaves.
- **So the tearing was probably not EchoFG alone**: the same wrong motion breaks FSR's frames more. It is the case a generator should degrade gracefully in, and ours does so only a little.
- **Softening where the motion is doubtful** (a setting, "Soften where the motion is doubtful", 0.6 by default: the frame leans toward the plain mix of the two real frames where the moved pair disagrees, up to 80 % times the setting): +0.1 dB at normal motion, +0.2 to +0.25 dB at 3 to 4x. It does **not** remove the blocky wall patches: those cells' vectors are wrong but the moved pair agrees (a periodic texture: the aperture problem), so nothing flags them as doubtful.
- **Where the error is at 4x motion** (`interp_flow_lab.py` on the same frame, the vector field, the cells nothing reaches and the error drawn): the estimator's vectors are *not* wrong: they match far better than any single global shift (match error 13 to 21 against 40 to 67, and 50 to 75 for no motion). What makes the frame hard is parallax: near trees move 335 px, far ones 100, the character 0, so 56 % of the cells are real disocclusions (hundreds of pixels wide), and the pair cannot agree on content that one of the frames does not show. How those holes are filled decides the look.
- **Tried and dropped: preferring the slower (background) vector in a hole** (a cost per pixel of the vector's length, 0.1 to 1 level a pixel): it lowered every score (fast clip 27.87 -> 27.69 -> 27.44; no better at 3 to 4x). The agreement test stays.
- **Next**: the real frames from the owner's own session ("Record what is shown", then Ctrl+Shift+F1 within a few seconds of a glitch: his log said the recorder was armed but nothing was saved); then what to try for the periodic-texture cells (the vector field checked against its neighbours before the synthesis; a motion limit that blends outright when the average vector is a large part of the picture's width).

## The plan

Not a rewrite of FSR's job in one go; stages, each measured offline against `nr_fgeval`'s FSR number (30.4 dB on Silent Hill f) and then judged by eye live against LSFG (the only judge of "better than LSFG"):

1. **Our estimator's flow into the lab** (`nr_fgeval` can write it; the lab reads it): test synthesis ideas on motion that is already good, instead of block matching. This isolates the synthesis question.
2. **Synthesis with occlusion handling**: forward-backward consistency of the flow marks covered and uncovered pixels; there take the frame that sees the pixel, elsewhere the motion-compensated pair; both from public methods (bidirectional motion-compensated interpolation, consistency checks).
3. **Static-interface protection** with a threshold (a pixel where the two frames agree locally is copied, not moved): LSFG has this as a parameter (0.5); ours is a measurable rule, scored on a HUD clip.
4. **Features and confidence for the matching** (a few cheap channels instead of luma alone; a per-level confidence), the part our estimator lacks (docs/lsfg-vk-study.md, "What this means for us").
5. Only then shaders in the addon (`FgEngine`), behind the existing off-by-default frame generation switch (`NR_FRAMEGEN=OFF` in releases until the owner says).

## Not decided / to ask

- How much of the per-level chain to share between generated frames (LSFG recomputes the matching per timestamp; we could estimate once and scale in time for the multiplier, cheaper, a little worse in curved motion).
- Whether the target is "closer to LSFG on its weak spots" (halos, UI shimmer) or "FSR-class on top of our estimator, plus our own occlusion rules"; the plan above does the second first.
