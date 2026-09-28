# lsfg-vk: a look under the hood of LSFG 3.1

Studied 2026-09-27 from https://git.lsfg-vk.dev/lsfg-vk (PancakeTAS), commit 2a5dc88 of the same day.
lsfg-vk runs Lossless Scaling's frame generation (LSFG 3.1) on Linux as a Vulkan layer.

## The license, and what "using code" means here

lsfg-vk is **CC BY-NC-ND 4.0**: attribution, **non-commercial**, **no derivatives**.

| What | Allowed? |
|---|---|
| Reading it to understand how LSFG works (this note) | Yes. Copyright covers the code's expression, not the ideas, algorithms or pass structure it reveals. |
| Writing our own code, in our own words, for the same ideas (a feature pyramid, coarse-to-fine refinement, a flow-scale setting) | Yes. That is ordinary engineering from public knowledge. |
| Copying or translating its code (C++, its Mipmaps.hlsl, its pass tables) into our repo, even rewritten line by line | **No.** That is a derivative; ND forbids sharing derivatives at all, and NC forbids any commercial use. |
| Linking or bundling lsfg-vk itself | No (same reasons), and it is Vulkan/Linux anyway. |
| Loading LSFG's own shaders at run time from the user's installed `Lossless.dll`, as lsfg-vk does | Not an lsfg-vk license question at all: those shaders are Lossless Scaling's. We would ship nothing of theirs (the owner's rule: we can build against anything, we just can't ship the DLL). What remains is Lossless Scaling's terms and our relationship with its developer. **Owner's call; not done.** |

To be discussed with the owner: where the line sits for us in practice. The working rule until then: **read, learn, write our own; copy nothing.**
(Not legal advice.)

## How lsfg-vk runs LSFG

- It does **not** reimplement LSFG. `lsfg-vk-pipeline/src/library/dll.cpp` walks the PE resource table of the user's `Lossless.dll` and pulls each compiled shader out by resource id; `library.cpp` patches a few words of the bytecode (capabilities and the like) before creating the Vulkan pipelines.
- It has three shader maps (base, quality, performance) and a half-precision option.
- Its only shader of its own is the mipmap pass.
- The pass graph is written out in `src/pipelines/v3_1.cpp`; the rest of the code (layer, UI, config, CLI) is plumbing.

## The public interface (`include/lsfg-vk/lsfgvk.hpp`)

- `Context(instance, width, height, flowScale, performanceMode, depth, colorSpace, transferFunction)`.
- Inputs: **two real frames only** (an array of 2, frame n goes in slot `n % 2`). **No depth, no game motion vectors.** Same starting point as our addons.
- Output: one image per generated frame, synchronised by a timeline semaphore. The caller asks for as many generated frames between two real ones as it wants: **adaptive frame generation is scheduling on top of one generator**, not a different algorithm.
- Formats: RGBA8, A2B10G10R10 or RGBA16 (the "HBD" variants for HDR), sRGB or other transfer functions.
- `flowScale` is a separate setting from the output size (Lossless Scaling's "Flow scale" slider): motion is estimated at a reduced resolution, which is one reason LSFG stays cheap at 4K. (Where exactly it enters the extents was not traced.)

## The LSFG 3.1 pass graph

`mul` = 2 in quality mode, 1 in performance mode (it doubles the width of the intermediate feature images).
The names are the shader names; what each stage does is **our reading**, not stated anywhere.

**Pre-pass (once per real frame)**
1. `mipmaps`: both source frames → a 7-level **R8 luma** pyramid.
2. `alpha0`–`alpha3`, at each of the 7 levels: feature extraction. The outputs grow to `2·mul·3` images, so this is several feature channels per pixel, not plain luma.
3. `beta0`–`beta4`: one chain producing 6 images, later read one per level by the main pass. Probably a per-level comparison of the two frames (a cost or confidence).

**Main pass (per generated frame; the graph is split here)**
4. For each level from coarsest (i = 0) to finest (i = 6): `gamma0`–`gamma4`, reading that level's alpha features, the **previous (coarser) level's gamma result**, and that level's beta image. This is the **coarse-to-fine motion estimate**; level 0 runs a "Special" variant with no coarser input.
5. At the three finest levels only (i ≥ 4): `delta0`–`delta4`, chained from level to level (refinement), then `epsilon0`–`epsilon4`, also chained (likely occlusion / which frame to trust, or the blend weights).
6. `generate`: reads the two source frames, the finest gamma, delta and epsilon results, and writes the generated frame at full resolution (RGBA8 or the HDR variant).

## What this means for us

- **Our motion estimate already has the same skeleton**: a 7-level luma pyramid, refined from coarse to fine (`flow_estimator.cpp`: block vectors per level, a stray penalty per level). So the pyramid itself is not what we are missing. (The earlier chat message saying ours "works mostly at one scale" was wrong.)
- **What LSFG has that we don't:**
  1. **Features instead of raw luma.** It matches several channels per pixel (alpha), not one luma value. That is more robust in fast turns, on flat textures and with lighting changes. A few cheap features of our own (luma gradients, a local contrast / census-style signature) is the obvious next experiment for our estimator.
  2. **A per-level confidence image** (beta) feeding every level.
  3. **Extra refinement at the three finest levels only** (delta, epsilon): full-quality work where it pays, cheap levels elsewhere.
  4. **A synthesis pass designed with its own motion estimate**, instead of handing vectors to a third-party interpolator. Our FSR 3.1 path's weak spot (its optical flow gives up in fast turns and it falls back to a blend) is exactly where an estimator plus synthesis built together wins.
  5. **Flow scale as a user setting** (quality vs speed), plus a performance mode.
- **Adaptive frame generation**, when ours is good enough: a scheduler that picks how many frames to make and when to show each one. `framegen11.cpp` already computes the presentation times; nothing about the generator has to change.
- Frame generation stays out of releases (`NR_FRAMEGEN=OFF`) until it makes real progress; this study changes the plan for that work, not the release.

## Next experiments (when frame generation work resumes)

1. Feature matching in our estimator (gradients / a census signature beside luma), scored with `nr_fgeval` on the recorded turn clips (band score, worst frames).
2. A confidence output per level, used by the lean pass in the upscalers and by the guard in frame generation.
3. A flow-scale setting for our estimator, measured for speed and quality at 4K.
4. Only then, a synthesis pass of our own to compare against FSR 3.1's.

## Feature matching for our estimator: the concrete plan (2026-09-27)

Today `flow_estimator.cpp` scores a candidate vector by the plain sum of absolute luma differences over an 8x8 block (the block cost),
on a single-channel R16F pyramid. LSFG matches several feature channels per pixel. Steps, cheapest first, each scored with
`nr_sreval` (sharpness, coarse, and the new steadiness score) and `nr_fgeval` on the same turn and pan clips before the next:

1. **Zero-mean block cost**: subtract each block's mean luma (this frame's and the candidate's) before differencing. No new textures,
   a few instructions. Robust to lighting changes, fades, flashes and the gamma drift of the HDR-to-SDR conversion, where plain luma
   pulls vectors toward whatever happens to have the same brightness.
2. **A gradient channel**: store (luma, gradient magnitude) as RG16F in the pyramid and add a weighted gradient term to the block cost.
   Edges then dominate the match, flat texture (sky, water, fog) does not, which is where fast turns lose the estimate.
3. **A confidence output per level** (LSFG's beta has this role): the ratio of the best block cost to the second best. It could
   replace the distrust heuristic in the lean pass and feed the guard in frame generation.

The upscalers gain from 1 and 2 as well: their "Steady in fast motion" distrust mask and lean pass come from this estimator.

### Step 1 tried (2026-09-27): the zero-mean block cost

`FlowEstimator::SetMeanWeight` (and `meanweight=N`, percent, in `nr_sreval` and `nr_fgeval`): below 100 the blocks' shape is compared
with their average brightness taken out, and the brightness difference counts only that share. At 100 (the default) it gives exactly
the old scores. On the Silent Hill f recording (a turn, frames 60-99, and its start, 0-39; 40 frames, shrunk 1.5x):

| | upscaled | coarse | steady |
|---|---|---|---|
| DLSS L, turn, 100 / 25 / 0 | 32.82 / 32.84 / 32.86 | 43.18 / 43.29 / 43.33 | 30.11 / 30.12 / 30.14 |
| FSR, turn, 100 / 0 | 33.44 / 33.44 | 45.47 / 45.46 | 30.59 / 30.59 |
| FSR, start, 100 / 0 | 34.45 / 34.44 | 47.64 / 47.55 | 31.58 / 31.57 |

Neutral to slightly better: this recording has no lighting changes, which is what the zero-mean cost is for. Default left at 100 until
a recording with light changes (World of Warcraft spell effects, a flash, a fade) shows a gain. Next: step 2, the gradient channel.

### Step 2 tried (2026-09-27): the edges in the block cost

`FlowEstimator::SetGradWeight` (`gradweight=N`, percent, in `nr_sreval` and `nr_fgeval`): each pixel's step to its neighbours, this
frame's against the frame before's, blended into the block cost; taken from the pixels the cost already reads (the checkerboard search
uses steps of two), so no new textures and no extra reads. 0 (the default) gives exactly the old cost.

Upscaler (DLSS L, turn, 40 frames): gradweight 0 / 30 / 60 / 60 with meanweight 0: 32.82 / 32.84 / 32.85 / 32.85 dB (coarse 43.18 /
43.25 / 43.24 / 43.27, steady 30.11 / 30.13 / 30.13 / 30.14): a hair better, within noise.
The motion itself (`nr_fgeval mvcheck=1`, frames 50-109, the frame before moved half way by the vectors against the frame between):
gradweight/meanweight 0/100 19.84 dB, 60/100 19.83, 60/0 19.79, 100/0 19.75: **slightly worse**.
So on this fast turn the matching cost is not the weak spot; what limits the estimate there is more likely the search's reach and the
parts that come into view (occlusion). Next candidates: LSFG-style confidence (step 3), and a wider search at the coarsest levels.

In the addon: "Motion by shape (test)" in the upscalers' panel (setting `motionShapes`, off by default) sets meanweight 0 and gradweight
30, for trying live where the light changes (World of Warcraft).

### Where the estimate is limited (2026-09-28)

`nr_fgeval mvcheck=1` on the Silent Hill f turn (frames 60-83): the picture moves about 100 px every two frames; the frame before, moved
half way by our vectors, scores about 25 dB against the frame between, where the frame before as it is scores about 21.5. Letting each
pixel look further (reach 8 -> 24 px) changes nothing (25.02 -> 24.96, 25.92 -> 26.02). So neither the pyramid's reach nor the block cost
(steps 1 and 2) limits it: what is left is what comes into view and motion blur, and in such fast turns the upscalers lean on the frame
anyway. The estimate's next gains are for frame generation (occlusion handling, a synthesis of our own), not for the upscalers.
