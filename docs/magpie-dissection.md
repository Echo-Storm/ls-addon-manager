# Dissecting the Magpie Experimental fork, and where we go further (2026-09-30)

Follows `docs/magpie-fork-study.md` (the first read). Source: https://github.com/SAOG0721/Magpie (branch `experimental`, a GPL-3.0 fork of Blinue/Magpie). **Reference only:** nothing here is copied, and nothing from
it enters this tree; what follows are our own notes on what its documents and sources do, and what we would build ourselves. About 1,035 files: `src/Magpie.Core` (201 files, the scaling engine), `src/Magpie` (the
application and its XAML UI), `src/Effects` (182 shader files: RAVU, Anime4K, CuNNy, NNEDI3, SMAA, FSR 1, NIS, CRT ...), `docs/experimental` (121 design and review notes, mostly Chinese), tests and build scripts.

## What it is, against what we are

Magpie captures a window and draws a processed copy full screen; the experimental fork adds neural and temporal processing (DLSS super resolution, FSR 2/3/4, XeSS, DLSSNR = Neural Rendering,
DLSS and XeSS frame generation, NVIDIA RTX Video) on top of Magpie's capture, presentation, cursor, overlay and effect-chain machinery. We are an addon *inside* Lossless Scaling, which already does capture,
scaling, frame generation and presentation: we add to its passes. That is why some of their work (capture methods, cursor, windowing, presentation pacing) is not ours to do, and some of ours (working inside
LS's frame flow, the recorder, the manager) has no equivalent there.

## Side by side

| Area | Magpie fork | Us | Verdict |
|---|---|---|---|
| Graphics card | One card for everything, chosen by the user, with fallbacks (`DeviceResources`); a "frontend" and a "backend" device on the **same** adapter separate processing from presentation. No cross-adapter work. | Follows Lossless Scaling's card (its Preferred GPU); the compatibility test tests that card (0.9.16). Issue #6. | **Open ground for both**: nobody splits work across two cards. |
| Motion for the upscalers | Choice of none ("zero contract"), AMD FidelityFX optical flow, NVIDIA's hardware Optical Flow (driver `nvofapi64.dll`, 4x4 / 2x2 / 1x1 grids, bidirectional, hardware cost); one shared result for every consumer, cached per target size. A built-in fallback: 5x5 search at half size on five luma taps (reach +-4 px). | Our own multi-scale estimator (6-level pyramid, global step, per-pixel, distrust mask, fast-motion share), 0.2-0.4 ms; LSFG's flow where available. | **Ours is stronger than their fallback**; theirs has vendor engines we do not (numbers in the study: not faster). Sharing: we estimate twice when NR and an upscaler are both on (small). |
| Scene cuts, resets | Every frame's guidance carries a reset reason: initialise, resize, scene change, capture interrupted, device recreated, long pause, provider failure; consumers clear their histories. AMD's runtime has its own scene-change shader (`optical_flow_scd`, in the DLL we ship). | The upscaler's and NR's histories are cleared only at start and on a rebuild. No cut detection, no long-pause reset. | **Measured: not a real gap.** `nr_sreval splice=<other recording> spliceat=N` (a hard cut between two WoW scenes, 1.5x): with the lean and DLSS's bias mask (the defaults) the first frame after the cut is already at the new scene's score (34.45 dB, steady 34.44): no ghost; plain model E loses 0.6 dB on the cut frame and is back within one; model L has a tail of about 5 frames (0.2-0.7 dB). The distrust mask (same frame, on the GPU) and Neural Rendering's luma check already do the job; a CPU reset would come a frame late. **Not built.** |
| Neural Rendering anti-flicker | Five modes (static accumulation, optical-flow accumulation with input validation, persistence with hysteresis, low-frequency temporal reconstruction), 1-3 passes; "not yet measured on real hardware". | Motion-compensated smoothing with luma validation, now trusting the history more where the input is unchanged; measured with `nr_nreval`. Persistence was tried offline and loses. | **Ahead on evidence**; they have multi-pass with per-pass controls, we have `passes` 1-4. |
| Frame pacing and latency | Front Edge Sync; Reflex (native low-latency markers); a presentation FIFO with clock and capacity gates; adaptive presenter for VRR; XeSS FG's XeLL pacing. | LS's own; our passes must not make its frames late (GPU priority, the model never blocks a present). | Mostly LS's territory. **Frame trace** (next row) is the useful part. |
| Diagnostics | A frame trace: fixed rings of timestamped events (capture, flow submit, effect calls, publish, present durations and intervals), exported as CSV when scaling stops, with a standard-library Python analyser (submit-interval distribution, longest 20 gaps, overlapping stages, duplicate frames); an in-app "recent issue" card with suggestions. | Aggregates every 300 frames (p50/p95, model ms, GPU start, composed/presents), the recorder for frames, the manager's diagnostics file. No per-frame timeline. | **A gap, and the one most useful for the pacing report.** Planned. |
| HDR | Canonical storage linear scRGB (80 nits = 1.0); PQ decoded to absolute nits; SDR and HLG embedded at their reference white; explicit HDR conversion effects; NVIDIA TrueHDR (SDR to HDR) through the RTX Video SDK. | The same contract (light, SDR white); the SDR view with a logarithmic roll-off for the model and our passes; 0.9.22/0.9.23 fixed the highlight specks. | Equal; **their SDR-to-HDR (TrueHDR) is something we lack** (licence: NVIDIA's). |
| Single-frame neural upscaling | NVIDIA RTX Video through the Video Effects runtime: VSR (quality 1-4) and denoise (8-11, high bitrate 16-19), loaded from the user's runtime; licence "unresolved" in their own inventory. Also CuNNy, RAVU, NNEDI3, Anime4K as shader networks. | None. Temporal upscalers only; we measured that without jitter they cannot add detail (issue #8: "only VSR gives the look I want"). | **The gap issue #8 asked about.** A user-supplied-runtime route (as with the NR model) is possible; nothing is downloaded or shipped without the owner's yes. |
| Frame generation | DLSS frame generation through NGX (2-4x multi-frame; flags for HUD-less, UI, UI alpha, bidirectional distortion field; Reflex), XeSS FG (2-4x, AMD or NVIDIA flow, XeLL pacing; GPL-adapted compatibility code from OptiScaler), duplicate-frame filter. | FSR 3.1 generation prototype of our own (out of releases); XeSS FG and FSR evaluated offline (`nr_fgeval`); we already fetch Intel's `libxess_fg.dll` and `libxell.dll`. Issue #9. | Their frame generation works on capture with estimated motion and zero depth, with the limits their README states. **A long project for us**; the XeSS route is nearest (same Intel licence as the XeSS runtime we ship). |
| Parameter UX | Each control marked Live or Restart, double-click resets, an input-validation test suite, parameter panel while scaling. | Panel applies live; per-game profiles; no validation matrix. | Small wins. |
| Licensing posture | GPL-3.0; NVIDIA and AMD runtimes kept out of public builds until cleared; own inventory of every component. | Not GPL; never ship NVIDIA's model or runtimes we may not; OptiScaler reference-only. | Consistent. |

## Dual GPU

Magpie has no dual-GPU path: `DeviceResources` walks the adapters (by the user's vendor/device id, then the rest, then WARP) and takes the first that creates a device; capture, processing and presentation are on that
card. The only shared resources (`OpenSharedResource`) are between its two devices on that one adapter. So a real dual-GPU design would be new for everyone. Ours already covers the useful case (Lossless Scaling's
card is the model's card, issue #6). The next step would be running the model on a *different* card from LS's (an idle second card when LS's is saturated, which is what we measured at the Durotar spot): frames in and
deltas out cross the bus (a 4K HDR frame is 66 MB: about 4 GB/s at 60 a second on PCIe 4.0 x16, which fits), through cross-adapter shared heaps; the model's output is already consumed one frame late, so the added
latency is hidden. A project of its own, worth doing only for someone with two strong cards; parked in the backlog.

## Motion, in one paragraph

Their half-resolution flow is a +-4 pixel luma search; ours reaches tens of pixels with a pyramid and refines per pixel, and produces the distrust mask the lean needs. Their real strength is the vendor engines
(NVOF, AMD's flow) and a shared, typed contract (motion, depth, confidence with metadata and reset reasons) consumed by every effect. We measured NVOF's published speed (1.0-1.9 ms a 1080p frame at 4x4 on an RTX 4090)
against our 0.2-0.4 ms shader estimate: not a saving. The contract and the reset reasons are worth having.

## What we do that they do not (kept, and to be kept ahead)

- Lean pass (the picture blended toward a plain resample by a distrust mask), Sharpness at rest, Steady sharpening with its motion trust band, Sharpen less in fast motion, FSR sharpening unified: measured against
  true frames (`nr_sreval`), none of it in their notes.
- Offline measurement of everything: `nr_sreval`, `nr_nreval`, `nr_composebench`, `nr_filter_lab.py`, the HDR checks and the automated suite with 48 model scenarios.
- Neural Rendering auto quality that goes straight to the fitting resolution, remembers it and backs off (every Nth frame) when the game's frames slow.
- The recorder and the offline tools built on it; the manager; the installer; per-game profiles.

## Plan (in order)

1. ~~Scene-cut detection and reset reasons~~: measured, not needed (see the table).
2. **A frame trace**: per-real-frame timeline (tap, submit wait, model start/done, compose, present interval) in a ring buffer, exported as CSV, with `tools/analyze_frame_trace.py`. For the pacing report and for us.
3. **A "what is wrong" card** in the panel from the same numbers (GPU saturated, model at its floor, frames slow, plain frames from the A/B toggle): the answer that would have saved a log read today.
4. Research, each needing the owner's yes before anything is downloaded: a user-supplied NVIDIA Video Effects runtime for single-frame VSR (issue #8); XeSS frame generation live (issue #9); a second card for the model.
