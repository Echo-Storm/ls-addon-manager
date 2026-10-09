# A frame generator of our own for Lossless Scaling: what we know, what the lab says, the plan (2026-10-09)

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
