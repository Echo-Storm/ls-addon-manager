# Study: SAOG0721/Magpie ("Magpie Experimental"), 2026-09-30

A read-only study of https://github.com/SAOG0721/Magpie, an unofficial experimental fork of Blinue/Magpie (a Windows window-scaling tool) that adds "colour-only" DLSS,
FSR 2/3/4, XeSS, DLSS neural rendering (DLSSNR), frame generation and RTX Video on top of Magpie's capture and effect system. It is GPL-3.0.

**Boundary.** Read for ideas only, like OptiScaler: no code, shader or text from it is copied into this project, and nothing GPL enters our tree. What follows are our own
notes on what their documents say and what we could test with our own code. Their design notes are in Chinese (`docs/experimental/design`, `reviews`, `todos`); the ones
read were `design/20260914-dlssnr-temporal-stabilization-routes.md`, `reviews/20260908-v0.6.7-r2-dlss-pipeline-review.md`,
`reviews/20260908-v0.6.7-r2-source-cost-and-xess-comparison.md`, `docs/NVOFA_Application_Note_中文解读.md` and, for the parameters it sets, `src/Magpie.Core/DLSSSRUpscaler.cpp`.
Credit to the fork's authors for writing their reasoning down; a lot of it applies to us.

## Where we already agree

- Their DLSS super resolution on captured frames does what ours does: no camera jitter (0, 0), a zero depth texture, motion from an optical-flow estimate of the frames,
  auto exposure, the low-resolution-motion flag, the HDR flag, a "bias current colour" mask. They state the same limit we measured: without the game's own motion vectors,
  depth and jitter it is not equivalent to a native integration. Their default DLSS model for this path is J (Balanced); ours is E at 1:1 and L when upscaling, chosen from
  recordings (docs/frame-generation-research.md).
- For neural rendering they reached the same design as our delta smoothing: keep the model's *change* (output minus input), warp it along the optical flow, validate the
  history against the *input* (not against the change itself), blend adaptively, limit the history to the current neighbourhood's range. Their warning is worth keeping:
  never reject history just because `|current change - history change|` is large, or the model's own flicker is taken for new content. Ours matches the luma of the frame.

## What is new for us

### Neural rendering flicker (the owner's main complaint about NR)

Their note lists routes we have not tried, and says none of A to H has been measured in a game ("not yet tried on real hardware"). Our `nr_nreval` measures it offline:
it runs the model on any recording and reports the model's change where the game's frame did not move ("delta change where the game is still", in levels of 255).
Baseline today on WoW clip 23-54-01, frames 20 to 59, working scale 50: smoothing 0: **0.89** (along the motion 1.02, live path 1.37); smoothing 40 (the default): **0.64**
(0.77, 1.09). That is the number to bring down.

Routes to try against it, cheapest first:

1. **Frame-rate independent smoothing.** Their weight is `exp(-dt / 0.08) * match` from the real capture time, with history dropped after a 250 ms gap. Ours is a fixed weight
   per model run (0.4), so its time constant changes with the frame rate the model runs at. Time-based is the same look at any rate.
2. **Persistence with hysteresis (their F).** Keep the change's amplitude and a slowly changing "how long has this correction been present" separately; appear fast (about 60 ms),
   disappear slowly (about 180 ms); show `presence * amplitude`. Meant for corrections that flip between there and not there, which a plain average turns into a weak,
   permanent correction.
3. **Fast and slow histories (their C).** Two averages with a rule that switches to the fast one when the slow one is confirmed wrong.
4. **Low-frequency temporal filter (their G).** Average only the low-frequency part of the change over time and pass the high-frequency part through; cheaper storage
   (half size). Their own caveat: flicker in the high-frequency part still passes.
5. Not first choice: short-window median (D), frequency split with spatial filtering (E), changing the downsampling phase (H).

### Frame-time hygiene (things they did to cut CPU and GPU work)

- GPU timestamp queries and their per-frame readback were on all the time; they made the detailed timing an opt-in. We keep timestamp queries and a read per engine slot per
  frame (and the estimator's stamps). Worth sampling one frame in N, or only while the panel that shows them is open.
- Readback buffers mapped once and kept mapped; a wait event created once per instance, not per wait.
- Writing the composite straight into the output's unordered-access view where it is one (a copy and a full-size intermediate saved). Compare with our compose path.
- A fast path for "same size" guidance was tried and dropped when a probe showed one half-float value differing (0x001a became 0x0019): the lesson is to test that a shortcut
  is bit-identical (or say by how much it is not) before taking it. Ours are all checked against the same recordings.

### Hardware optical flow (NVOF), and why not yet

They use NVIDIA's Optical Flow Accelerator (fixed-function engine, Turing and newer; Direct3D 11 and 12; 4x4 grid on Turing, 2x2 and 1x1 on Ampere and Ada) for the motion
that feeds DLSS. NVIDIA's numbers for 1080p, 4x4 grid, on an RTX 4090 (Ada): SLOW 536 fps (1.9 ms), MEDIUM 1000 fps (1.0 ms), FAST 1296 fps (0.8 ms); SLOW uses the graphics
and CUDA engines too, so it is not free. Our shader estimate costs 0.39 ms (0.23 with motion sharing) at 4K output, 1.5x, per-pixel output. The hardware engine would take the
work off the shaders but is slower per frame and coarser (a 4x4 grid), and adds a dependency on the SDK, on the driver and on 8-bit input formats. Not compelling on the 4070 Ti
SUPER; worth revisiting only for a GPU where shader time is the limit, or as a source of a confidence signal (they use a bidirectional pass and its cost for that).

### Other

- **Frame pacing and latency.** They built a "Front Edge Sync" mode and native Reflex-based limiting so real frames reach frame generation at an even pace. Out of scope for the
  upscalers, but a reference if we ever look at LSFG's pacing (docs/lsfg-vk-study.md).
- **HDR.** Their validation compares colours after `c / (1 + |c|)` in HDR, our SDR-view approach for the estimate is equivalent in intent.
- **Parameter panel while scaling.** Each control is labelled as applying live or needing a restart; a double-click restores a default. Our panel applies live except where noted.

## Next steps (in BACKLOG.md)

1. NR: frame-rate independent smoothing, then persistence/hysteresis, measured with `nr_nreval` on the WoW and Silent Hill f clips (the "still" number above).
2. Engine: timestamp sampling, persistent readback mapping, event reuse (small CPU wins; measure with `nr_sreval bench`).
3. NVOF: no action; numbers above.

## What we tried from it (2026-09-30)

Measured with `nr_nreval` (the model's change where the game's frame did not move, levels of 255; smaller is better), on the owner's own recordings of a character screen (daylight foliage) and
of Durotar (red cracked ground), model at working scale 0.3.

- **Trusting the history more where the input is unchanged (their principle: validate history by the input, never by the change itself): adopted.** Where the frame's brightness matches the
  history's, the history weight rises to `1 - (1 - setting) / 4`. Durotar 0.72 -> 0.43 at setting 0.4, 0.40 -> 0.29 at 0.8; character screen 0.42 -> 0.20 and 0.21 -> 0.11. Default setting 0.4 -> 0.7.
  Not measured: ghosting or lag (the metrics cannot see it; the luma check is the guard, but a colour change with the same brightness would keep the old change for about twenty frames).
- **A wider downscale for the model's input (3x3 grid of bilinear reads instead of four): no effect** (0.03 either way): the input is not where the noise comes from.
- **Time-based smoothing weight (`exp(-dt/tau)`): not adopted.** The model's noise is new every run, so how much is averaged out depends on how many runs are averaged, not on how long they
  took; a per-run weight gives the same noise reduction at any frame rate (a time-based one gives less at 30 fps than at 60), at the price of more lag at low rates. Kept per-run.
- Still open: persistence with hysteresis (their F) and the fast/slow histories (C); each needs a second history channel.
- The GPU-based validation of the D3D12 debug layer (`nr_sreval debug=2`) reports an incompatible-layout message on the shared output texture in every pass, including the plain ones: it comes
  from the offline tool's shared texture, not from a pass.

## What the passes cost on the CPU and GPU, measured (2026-09-30)

- **Engine CPU** (`nr_sreval bench=400`, the engine thread, per presented frame): DLSS 0.3 to 0.5 ms, FSR 0.35, XeSS 0.27, whichever of our passes are on (0.16 to 0.35 ms between settings, within the tool's resolution): about 3 % of one core at 120 frames a second. The Neural Rendering tap costs 0.1 to 0.3 ms a real frame and its compose 0.02 ms of CPU. Sampling the timestamp queries or keeping readback buffers mapped would save microseconds: not done.
- **Neural Rendering compose GPU** (`nr_composebench`, 4K, RTX 4070 Ti SUPER): SDR 0.23 ms (0.29 with sharpening); HDR scRGB 0.59 ms (0.94 with sharpening). Arithmetic-bound by the view conversions (a logarithm and a power a channel each way), and the sharpening converted each of four neighbours again for every pixel. Now 0.56 and 0.83 ms (tile shared, round trips removed); the rest is the copy of the frame, the flow and delta reads and the conversions the result needs.

## Persistence with hysteresis and the other filters, tried offline (2026-09-30)

`tools/nr_filter_lab.py` runs candidate temporal filters on the model's own raw change from the owner's recordings (`nr_nreval smooth=0 dump=file`: per frame, the still-pixel mask and the change, at working
scale 0.3), and scores the flicker left where the game's frame stood still and the frames each filter takes to come 90 % of the way to a clean step of 0.02, 0.05 and 0.10 (a flicker is only better at the same
response). Durotar (raw 0.79 levels of 255; today's weight at slider 0.8, 0.95, leaves 0.091 = 12 %, 44 frames to a step) and the character screen (raw 0.52 -> 0.079, 15 %):

| filter | Durotar flicker | response (0.02 / 0.05 / 0.10) | character screen flicker |
|---|---|---|---|
| EMA 0.95 (today at 0.8) | 0.091 | 44 / 44 / 44 | 0.079 |
| EMA 0.97 | 0.063 | 75 / 75 / 75 | 0.057 |
| fast 0.6 and slow 0.97 histories, slow reset to fast after 3 frames apart by > 0.02 (their C) | 0.084 | 75 / 12 / 28 | 0.068 |
| the same with slow 0.98 | 0.074 | 88 / 16 / 41 | 0.059 |
| stateless robust average, weight 0.97 -> 0.5 as the new value leaves the history by 0.02 to 0.06 | 0.078 | 75 / 54 / 32 | 0.064 |
| variance-adaptive (a running noise estimate per pixel) | 0.076 | 53 / 43 / 34 | 0.064 |
| **persistence with hysteresis (their F), 60 ms up / 180 ms down** | **0.322** | 7 | |

Persistence loses clearly: the model's noise is amplitude jitter, not corrections flipping on and off, and the presence channel adds flicker of its own. The others slide along one frontier, at best 10 to 20 % less
flicker at equal response for a second history texture (fast/slow) or a noise channel (adaptive): not perceptible on a residual of 0.1 to 0.3 levels, so not built. What dominates what is seen: on the Durotar clip **the
game's own frames flicker at about 7 levels** (`nr_sreval`, "flicker" of the game's frames) against 0.3 to 1.0 from the model; sharpening amplifies that (the upscaler's sharpening 0.45 gives 113 % of it), and a second
sharpener after the upscaler, Neural Rendering's own `sharpen` in the compose pass, stacks on top.

## Follow-up 2026-10-09: the fork's 0.6.9 (released 2026-10-02) and what else moved

Same boundary as above: read for ideas only (GPL-3.0), our own notes, nothing copied. Read: the release notes (v0.6.9-experimental) and `docs/experimental/reviews/20261001-v0.6.9-duplicate-frame-filter.md`.
The repository is very active (about 100 commits between 09-30 and 10-03, 120 forks).

### Duplicate-frame handling (the same problem as our repeat-source note, docs/capture-card-sources.md)

- They compare each captured frame with the one before **on the GPU, exactly, channel by channel** (alpha too in HDR), a group reduction that publishes a "differs" flag, read back with a **synchronous Map**.
  They considered an asynchronous ring and left it: it needs a snapshot of the input, strict pairing of result and frame, back-pressure and extra latency, and they had no measurement showing a net gain.
  That is the same design as our probe (a compare on the card, one float read back) except that ours is asynchronous (a few frames late, never waiting) because ours only *reports*; theirs *acts* on the answer.
- What they do with it is the part we do not do: **a repeated input skips the effect chain** (the shaders, the HDR conversion and the SDK work) and the previous output is shown again; each effect keeps a "content version", and a downstream
  effect recomputes only when its input's version changed. Where an old input must be processed again anyway (a minimum frame rate, a cursor refresh), the temporal upscalers get a zero-motion guide and an SDK **reset** for it,
  so a repeated frame does not feed the history as if it were new content. Neural Rendering decides from the captured guidance whether its cached total correction can be rebuilt without running the model.
- For us this is a performance idea with a clear case: **a 30 fps source in a 60 or 120 Hz feed makes Neural Rendering (8-12 ms at 4K) and the upscalers run twice or four times on identical pictures.**
  Skipping the repeats would save half to three quarters of that cost for capture-card users (nitro's GTA VI plan), and identical pictures would get identical output (no flicker between repeats).
  Not built; the open problem is knowing, at the TAP, that this frame repeats the one before: our compare answers a few frames late. With a stable repeat count L and the phase known from the stays the detector already keeps,
  the model could be run on the first of each group and the result reused for the rest. Backlog.
- They log *why* each frame was classified as it was (accepted, no frame, interrupted, pixel-duplicate, skipped check, unfiltered, new sequence, colour change, ...) in their frame trace; ours has the taps and presents but not a reason. Cheap to copy as an idea.

### Neural rendering controls (their "detail control")

- Their 0.6.9 simplifies the NR correction into a few intensities (total, chroma, luma total, shadows / structure, highlights / glow), applied **once to the combined correction after all passes**, with advanced protections (hue, shadow, highlight protection,
  over-correction suppression, low- and high-frequency strengths) and a **diagnostic view** that shows the correction itself. The diagnostic view is the idea worth taking: a debug view of "what the model changed" (our `nr_nreval` writes a flicker heat map offline, but a tester
  cannot see it live). It would have made issues like #18 (ghosting with frame generation) easier to describe. Backlog.
- Their multi-pass NR (1, 2 or 3, default 1) and "input resolution 25-100 %" match our passes and model scale; their warning that a very low input resolution gives artefacts matches ours (the floor of 0.25).

### Smaller

- "Frame rate and refresh": content frame rate follow-source / auto / custom, pacing Front Edge / Async / NVIDIA Reflex, cursor refresh policies, idle redraw. Lossless Scaling covers pacing; nothing to take.
- A fix for choosing the wrong display adapter in **virtual-display setups (Sunshine)**: render on the real GPU, not the display-only one. Our card choice follows the device Lossless Scaling dispatches on, so this should not apply; worth a thought if a streaming user reports the wrong card.

### Other repositories looked at (2026-10-09)

- **OptiScaler** (GPL-3.0, very active, 0.9.4 in July): this week's commits are menu work, DX11 frame-generation fixes, a memory-leak fix when NvngxFFX errors, and "late latching for reprojection" in its frame generation. Nothing that bears on issue #11 beyond what we have (the Agility core).
- **lsfg-vk** (the Linux port of Lossless Scaling's frame generation; last push 2026-09-05, 2.0.0-dev29 on 08-27): no recent change to study; CC BY-NC-ND, read-only as before.
- **Upstream Magpie** (Blinue, GPL-3.0): 0.13.0-preview02 on 10-01 (only a start-up fix since preview01); nothing for us.
- **Beware:** most of what a search for "lossless scaling" or "DLSS5" returns is search-bait with no code (generic READMEs, some with hundreds of stars). Two to be careful with if users ask: `StormHatchet/LossLess-Scaling` (98 stars, no description) and `Fastbrasecret8/DLSS5-Manager`
  (387 stars, "one-click DLSS 5 manager"): not read, not vouched for; nothing from them should be recommended or run.
