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
