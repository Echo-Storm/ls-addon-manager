# Backlog

Everything worth doing that is not done yet, so nothing is lost between sessions. Newest context first within each group. Update it when an
item is done (move it to the changelog) or when a new one comes up. Dates are when the item was noted.

## Neural Rendering: the glow beside bright UI (2026-10-01)

- **Done: the dark guard** (compose11, commit d956b1f): a pixel near pure black takes no positive change. Owner, live: "significantly better".
- **Tried and dropped: an edge-aware stretch of the model's result** (weights of the four nearest delta pixels by how much the frame at each looks like this pixel, with the point guide and with the block average). It cost about 0.5 ms per presented frame (compose 1.40 -> 1.92 ms at 4K HDR) and changed nothing: the strip beside the box stayed at G 0.87 of 255 at model scale 0.25 and 0.47 at 0.6. The probe (`nr_nreval piclog=1`, temporary, not kept) shows why: in pure-black blocks the model's own green change is about 1.2 levels of 255 two to seven blocks from the bright content and 0.14 far from it, so the glow is what the model outputs around bright things, not the stretch smearing it. Do not retry stretching tricks for this.
- **Done: the dark guard's range is a setting** ("Dark guard", 0.040 by default, 0 to 0.15). Open: a live look at 0.06 to 0.10 on dark HUD panels from the people who see a glow left; no default change until someone asks.

## Single-frame neural upscaling for the static look (issue #8), researched 2026-10-02

- **What the reporter wants** is NVIDIA's RTX Video Super Resolution (VSR), a trained single-frame network that invents edge detail; our temporal upscalers cannot without jitter (measured; answered on the issue, "Sharpness at rest" 0.3 to 0.5 is the setting that helps).
- **What it takes, found by search:** the RTX Video SDK (https://developer.nvidia.com/rtx-video-sdk). The download needs a free NVIDIA developer sign-in and acceptance of the RTX Video SDK licence; its components (`nvngx_vsr.dll`, `nvngx_truehdr.dll`) are NVIDIA's and cannot be redistributed. So the only route is the one the Neural Rendering model uses: **the person downloads it themselves** and the addon loads it from a folder. Nothing of NVIDIA's is shipped or fetched by us.
- **What we have:** `external/ngx/include/nvsdk_ngx_defs.h` only defines the availability flags (`NVSDK_NGX_Feature_VideoSuperResolution = 5`, `VideoSuperResolution.Available` and the driver-version keys). The create and run calls, and the model, are in the RTX Video SDK itself.
- **Measured 2026-10-02 (the owner installed the Video Effects SDK 1.3.0.0 x64 and its VideoSuperRes feature in `external/vfx_x64`; `nr_vfxprobe`, tools/vfx_probe.cpp):** VSR modes Low to Ultra are the RTX VSR itself (`nvngx_vsr.dll`, modes 1-4, plus 8-19 denoise / deblur / high bitrate, 21 and 23 streaming). GPU time per frame on the 4070 Ti SUPER: 2560x1440 to 3840x2160: bicubic 0.9 ms, Low 2.3, Medium 2.9, High 6.4, Ultra 8.4; 1708x961 to 2562x1442: Low 1.4, Medium 3.5, High 13, Ultra 20. Detail (mean edge strength, truth = 100 %): bicubic 65-78 %, VSR Low 82-93 %, higher modes no better than Low. PSNR the same as bicubic (it sharpens, it does not match pixels). Steadiness over 40 consecutive frames of the owner's moving WoW clip (the input a 1.5x box shrink of each frame): the result's change where the game's picture did not change (still, levels of 255) bicubic 1.65, Low 2.11 (+28 %), Medium/High/Ultra 2.46-2.49 (+50 %); the error of the result's change against the truth's (moving) 3.16, 3.97 (+25 %), 4.3-4.5. Our DLSS upscaler on the same frames by `nr_sreval` (a different flicker measure, against its own plain stretch): flicker 99 % of the stretch, detail 110 %. So VSR Low takes about twice the detail gain of the DLSS pass for about 25-30 % more frame-to-frame change (under 1 % of the range); Medium to Ultra only shimmer more. Low is the mode. Not measured: a real lower-resolution render (the input is a shrink of the truth), HDR (VSR takes 8-bit or RGB10A2), the cost of getting a D3D frame in and out of CUDA (the SDK works on CUDA buffers, with D3D transfer helpers in nvTransferD3D11.h), and a live look.
- **If it is built:** a separate addon, runtime from the user's folder (never shipped; NVIDIA's licence and models), VSR Low by default, optional use only where the picture is still (a motion gate from LSFG's flow, as the steady passes do), SDR view for HDR. First step: the D3D11 to CUDA hand-over cost on a 4K frame.

## Known, rare (2026-10-01)

- **A fault inside NVIDIA's driver in the HDR upscaler host scenarios**: `scaler_hdr_scrgb`, run with three others side by side, ended with an access violation in `nvwgf2umx.dll` (a null read) about once in 20 to 60 runs (5 in roughly
  100); 0xc0000005 twice and 0xc0000409 once; always right after the host switches the frames to scRGB (new shared textures, the engine started again on them). Our own guarded passes caught one (the log: "exception 0x... in the DLSS
  scaler"; it now says the module, offset, address and step). The test host now prints the faulting module, offset and stack for any thread's crash ("[hosttest] FAULT"), which `tools/` loops can catch (see the crash hunt script idea:
  run the four HDR scenarios together until one fails). Not seen in 40 clean runs since; not seen in live use. If a user reports a crash in `nvwgf2umx.dll` while the upscaler starts on HDR frames, this is the lead.

- **Issue #11, a lead from the reporter (2026-10-01):** on an RX 6600 XT the FSR 4.1.1b INT8 runtime crashes in our addon but works for them in OptiScaler with `FsrAgilitySDKUpgrade` configured. The addon's own D3D12 device uses the system D3D12 and no Agility SDK; if the INT8 build needs a newer D3D12 core that would explain it (a guess). Asked them for the files and versions they use; to try: ship the Agility SDK beside the FSR addon and create the device through it.

## GitHub issues (state 2026-09-30, all answered)

- #7 DLSS Upscaler "FeatureNotFound" with NR on: fixed in 0.9.16 (NGX search paths union, ngx_paths.h). Awaiting the reporter.
- #4 Performance tab on AMD: fixed in 0.9.16 (Windows counters); awaiting a report from a real Radeon (only tested against NVML here).
- #6 two NVIDIA GPUs: the compatibility test uses the card LS runs on (0.9.16). Running the model on a different card from LS: not supported (needs cross-adapter copies); the reporter was asked for the second GPU model and logs.
- #5 antivirus flag (Defender cloud verdict `Trojan:Win32/Tecabans.STV!cl`): docs/antivirus.md written; our scans are clean. **Code signing** is the real fix: options to weigh with the owner: SignPath Foundation (free for open source, needs an application and a CI build), Azure Trusted Signing (about $10 a month; check eligibility), a normal certificate. Also submit the zip at microsoft.com/wdsi/filesubmission (needs the owner's Microsoft account).
- #9 (2026-09-30) asks for XeSS FG / DLSS FG in place of LSFG's interpolator, pointing at the Magpie fork. Answered: we are looking at it; that code is GPL-3.0 so ours would be an own implementation from the published research (docs/magpie-fork-study.md); no date promised. #8 (softness at rest): answered 2026-09-30 (Sharpness at rest, Steady sharpening, single-frame neural upscaler not near-term).
- Also 2026-09-30: the repository has one fork (ardesart, created 09-28, no changes of its own).
- #1, #2, #3 answered on 2026-09-28 (#3 fixed in 0.9.15). Left open for the reporters.

## State 2026-09-30 (for whoever picks this up; everything below is in CHANGELOG.md "Unreleased" unless it says 0.9.2x)

Released today: 0.9.20 (bug sweep), 0.9.21 (Steady sharpening, Sharpness at rest 0.5) and **0.9.22 (a major HDR bug fix, steadier Neural Rendering, Sharpen less in fast motion, FSR sharpening; issues #1 and #3 closed)**; the full suite with the model scenarios was green (`LS_DIR='D:\Utilities\Lossless Scaling' tools\run_addon_tests.ps1 -All`). The items below are in 0.9.22; several have never been seen by eye:

- **HDR highlight specks fixed** (sharpening and the picture controls could turn a bright pixel into a 10,000-nit speck; found with the new `nr_sreval hdr=1`; a test is in the NR suite). Significant: worth releasing.
- **NR**: auto quality goes straight to the fitting resolution, remembers it (`autoScaleLast`) and tightens when the game's frames are slow (the ramp was the owner's "lighting changes"); smoothing trusts the history more where the picture is unchanged, default 0.7.
- **Upscalers**: "Sharpen less in fast motion" (0.5), Steady sharpening on (0.6) and one fused pass with a motion trust band, FSR sharpens with our pass (RCAS left 73-83 % of the detail), motion estimate shared between generated frames, a 10-bit history.
- **Owner's live log (2026-09-30)**: WoW at 2562x1442 -> 3840x2160 (x1.5), HDR, frame generation x2; with Neural Rendering on, the game fell from 60 to about 35 fps at a heavy spot (Durotar): the card is saturated, not just the model. The owner's config was changed for testing (backup `addons/config.json.bak-wowtest-20260930`): NR on, auto quality OFF, working scale 0.3, smoothing 0.8.
- **Not yet seen by eye**: Sharpen less in fast motion, the NR smoothing change, FSR's new sharpening, the fused pass in HDR (the offline HDR checks pass), the auto quality changes.
- **Open, in order**: (1) NR persistence / fast-slow histories: tried offline 2026-09-30 (`tools/nr_filter_lab.py`, docs/magpie-fork-study.md): persistence loses, the rest gain at most 10-20 % at equal response: NOT built, parked; (2) engine CPU wins (timestamp queries sampled, persistent readback maps, event reuse); (3) HDR for `nr_nreval` (the NR eval still converts to 8 bit); (4) a release once the owner has looked at it; (5) Steady sharpening / movecut defaults are the owner's eyes to confirm.
- Tools: `nr_sreval` (hdr=1, debug=1|2, bench=N, movecut, steady, flowreuse ...), `nr_nreval` (stable=, lightlog=1), `nr_lsrec make hdr=1`, `tools/sr_default_grid.py`, `tools/sr_option_check.sh` are all in tools/README.md.

## Player feedback (kept as given; one person each, not measurements)

- **2026-09-30, a player testing Neural Rendering in WoW Forever (the new Skyborne zone), RTX 4090, 4K output, model resolution 50 %:** the most stable window-based DLSS 5 they have seen (they credit the "anti-ghosting"
  work: the motion-compensated smoothing of the model's change), and steadier than SAOG's Magpie fork, which they had thought the best for multiplayer and window-agnostic use. 35 % would run but loses too much detail
  (of note for older cards). Wants to try it in GTA VI (the "Extended Look") too, through an HDMI 2.1 capture card for a 60/120 Hz picture.
  - Their one complaint: **pacing and GPU utilisation get bumpy when the game drops below the frame rate Lossless Scaling is told to expect**; they suspect Lossless Scaling itself, and say adaptive frame generation smooths it.
    Worth checking against our side: the model runs on a thread of its own and waits for the GPU (see the "GPU start +N ms" in `DLSS5NR01.log`), and the new auto quality tightens its budget when the game's frames slow down (0.9.22), which may help.
    Needs a log from them (`DLSS5NR01.log`, `DLSS4DLAA.log`) while it is bumpy, and the game frame rate at the time.
  - **Their logs came back (2026-10-01, RTX 4090, 4K HDR 1:1, DLSS Upscaler preset M then E, frame generation x2, 120 Hz screen, game capped at 60):** the game's own frame times are steady (p95 within 2 to 5 ms of p50 in all 14 windows,
    base 26 to 51 fps), DLSS costs 1.4 to 1.6 ms of GPU per presented frame (the motion estimate 0.55 to 0.69 of it), no picture shown twice, 2 presented per real frame throughout, the Neural Rendering model 12.2 ms at their size and kept up (4
    skipped in 1200). Nothing on our side is bumpy. What matches "terrible at 40 to 59 base, clean at 25 to 39" is the display: x2 gives 80 to 118 fps, which does not divide into 120 Hz (uneven repeats), while 30 x 4 and 60 x 2 do. Suggested to them:
    Lossless Scaling's adaptive mode targeting 120, or a base that divides 120 (40 x 3, 60 x 2), or VRR. Their build was 0.9.23 (no frame trace); the next release has it, to confirm with the presents.
  - Use to keep in mind: a capture-card source (a window showing a capture feed, HDR or not) is a real use case for Neural Rendering, not only games rendered on the same PC.

## Released

- 0.9.18 (2026-09-29, v0.9.18 at 8d1af4b): the recorder is one setting for all the addons (mirrored into each addon's config, followed twice a second).
- 0.9.17 (2026-09-29, v0.9.17 at f813ad1): the upscalers start in ~1 s again (search shader compile 6.4 s -> 0.5 s; the shape cost is a background-compiled variant).
- 0.9.16 (2026-09-29, v0.9.16 at 4ef0aed): issues #4, #5, #6, #7 (see above).
- 0.9.15 (2026-09-28, v0.9.15 at 6d896fe): the HDR neon fix (issue #3, likely #1). Issues #1, #2, #3 answered on GitHub, left open for the reporters to confirm.
- 0.9.14 (2026-09-28, v0.9.14 at ae232c0): see the changelog. Confirmed live: the corner square in the upscaler addons; NR smoothing at 0.6 helps.

## Bug sweep (planned Tuesday 2026-09-29 morning)

- (Fixed 0.9.17) `scaler_bgra` failed because of the 9.5 s engine start; `--only` runs still need the baseline scenarios in the list.
- The corner square: the output swap chain is only learned while the upscaled picture is shown, so after a restart that begins on
  "original", the first toggle shows no square.
- Neural Rendering's changes in 0.9.13 (temporal smoothing clamp, NGX user count) were never tried live before release; the NGX count
  was seen working live on 2026-09-28 ("NGX left running (another addon still uses it)").

## Neural Rendering

- **Flicker while moving: measured 2026-10-02, no change needed; the owner thinks the original report was mostly their low frame rate plus things since fixed.** `nr_nreval` on the owner's WoW clip of 30 September (2562x1442, ~34 fps, frames 20-79, model scale 0.6): temporal smoothing 0 / 0.5 / 0.8 gives the model's change where the game is still 0.84 / 0.33 / 0.25 levels of 255, along the motion 1.30 / 0.64 / 0.50 (worst 5 % of blocks 2.90 / 1.56 / 1.26), steadiness 39.8 / 41.4 / 42.4 dB. At the owner's 0.5 the largest part left is the staleness of a delta moved along the motion: the live path is 1.45 levels off the frame's own delta (1.72 not moved), and a result two runs old (the model on every 2nd frame) is 1.80 against 1.54 one run old, on a delta that is 6.95 levels big on average. So smoothing already took most of it; 0.8 would buy about a quarter more at a cost in lag and ghosting. The default stays at 0.5.
- **From the Magpie-fork study (docs/magpie-fork-study.md, GPL: ideas only, no code):** measured baseline with `nr_nreval` (WoW clip 23-54-01, frames 20-59, scale 50): the model's change where the
  game is still 0.89 levels with smoothing 0, 0.64 with the default 0.4. To bring down, in this order: (1) DONE 2026-09-30: history trusted more where the input is unchanged (stable weight; docs/magpie-fork-study.md "What we tried"); smoothing by real time was considered and not adopted (the noise is per run); (2) persistence with hysteresis and (3) fast/slow histories: CLOSED 2026-09-30, tried offline with `tools/nr_filter_lab.py` (docs/magpie-fork-study.md, "Persistence with hysteresis"): hysteresis is worse (0.32 against 0.09), the others gain 10 to 20 % at equal response on a residual of 0.1 to 0.3 levels, not perceptible. What is seen is the game's own frames (about 7 levels of flicker on Durotar) and the sharpening that amplifies it; see the "Sharpening at 1:1" item under Upscalers.
- **Small engine CPU wins from the same study (measured 2026-10-01: the whole engine run costs 0.12-0.23 ms of CPU at 2560x1440, so sampling the timestamp queries would save a few hundredths of a millisecond: not worth the code; closed):** GPU timestamp queries and their readback every frame (engine and estimator) sampled one frame in N or only while the panel is open; readback buffers mapped
  once; wait events created once. Measure with `nr_sreval bench=N`.
- NVOF (NVIDIA's hardware optical flow) looked at: 1.0-1.9 ms a 1080p frame on an RTX 4090 at a 4x4 grid against our 0.2-0.4 ms shader estimate: no action (numbers in the study).
- Owner, 2026-09-30: Neural Rendering in general is too flickery for general use (their opinion); to circle back to after the upscaler work. Steady sharpening's running-average idea (docs/frame-generation-research.md) may carry over to the NR compose.
- Live with smoothing 0.6 (2026-09-28): "doing quite well", a weird flicker left on landscapes. The NR recorder was off (its own "Keep the last few seconds" box, off by default), so nothing was saved: next time a screen video plus an NR recording.
- **Tool ready: `nr_nreval`** (docs/frame-generation-research.md, last section). To use it we need a recording made **with Neural Rendering on and its own recorder on**, in daylight with foliage and terrain (the WoW clips so far were made with the DLSS Upscaler at night: the flicker is not in them). Look at the worst 5 % of blocks and the heat map.
- Earlier: the owner wants it improved (look and cost).
- Cost: in World of Warcraft at 4K the model takes about 8.5 ms a frame at half size (1912x1080), over the automatic budget (5 ms), and
  the automatic size is already at its floor (0.50): "auto: model resolution 0.50 -> 0.50". Frames over 20 ms: 54-82 %.
- Ideas: a lower floor for the automatic size (0.4 / 0.33) with the compose's upsampling of the change; run the model every other frame
  and carry its change along the motion (the compose already moves a result by its motion vectors); a quality score for NR offline (an
  `nr_nreval` like `nr_sreval`: the change's stability from frame to frame on a recording).

## Upscalers

- **Video memory (2026-10-01)**: the owner saw LS hold over 1 GB (15.2 -> 14.1 GB on close). Measured offline (HDR, 2562x1442): upscaler engine + DLSS 281 to 372 MB, Neural Rendering engine 298 to 412 MB; at 4K ~2.25x, plus the link's shared
  textures (5 x 66 MB in HDR at 4K). Ideas, none done: three shared pictures -> two (66 MB at 4K HDR, costs pipelining), NR's two full-size input copies, the lean's and the smoothing's targets shared; the DLSS feature itself is NVIDIA's. Looked at again 2026-10-01: NR's own textures are at the model's working scale (small); its few hundred MB is NGX's workspace, which follows the model resolution (Auto quality already moves it); the upscaler's own buffers at 4K HDR add up to under 200 MB. Nothing worth the code is left.
  The addons now log the process figure at engine start and with the periodic lines, to see it live.
- **Built, live as an option "Lighter upscaling of generated frames (test)" (off by default; the owner to try it by eye) (2026-10-01), cost: the DLSS side is 3.3 ms of GPU for every presented frame in the owner's HDR 1.5x log, two presented frames to each real one.** The upscaler on the real frames only, a cheap picture for the generated ones (`SrEngine::SetCheapNext`, `nr_sreval realonly=1`): measured on four clips, about equal in detail and flicker, 0.4 to 0.8 dB lower in fast SDR motion, steadier-score 1 to 3 dB lower in the HDR hall; the DLSS side 3.3 -> about 2 ms (docs/frame-generation-research.md, "The upscaler on the real frames only"). To do: know which presented picture is the real one, carry the flag with each queued run, set the present step from the time ratio, then the owner's eyes; an opt-in "Lighter upscaling with frame generation".
- **Built (2026-09-30, Unreleased): "Steady sharpening (test)"** = sharpening from a running average along the motion (docs/frame-generation-research.md, "Steady sharpening"): ~10 points less flicker at equal detail at rest, 1-2 in motion. Owner to try it by eye (Sharpening 0.5-0.7, steady 0.6-0.9); if liked, consider it on by default at 1:1.
- **Sharpening at 1:1 amplifies the game's shimmer** (docs/frame-generation-research.md, last section): the owner's 0.7 gives 2.3x the game's detail and 137-155 % of its shimmer. Ideas: temporal-aware sharpening (less where a pixel flickers without moving), a lower default at 1:1 (0.5 now), tell the owner to try 0.15-0.3. Needs the owner's eyes.

- **Owner to compare (next release): "Crisp edges when moving (test)"**: FSR 1's EASU instead of Catmull-Rom for the lean (visibly crisper edges on a crop; same shimmer and detail scores). If preferred, make it the default.
- The honest state of the upscalers (docs/frame-generation-research.md, "side by side"): without the game's camera jitter a temporal upscaler cannot add detail; in motion the picture is a resample (the lean), at rest it anti-aliases. On WoW walking every step toward a plain resample scores better; the default stays where the owner's eyes put it. A real gain would need a different source of detail (see below).
- **Ideas for real detail** (the point of a "definitive" tool; each has a test: `nr_sreval sample=point` gives aliased input, where a temporal method has something to unfold, area-averaged input has nothing; on both the plain stretch is the baseline to beat):
  1. (CLOSED 2026-09-29, with data: docs/frame-generation-research.md, "An oracle for the motion") Multi-frame super-resolution from the camera's sub-pixel drift: even with oracle motion (`nr_sreval oracle=1`) DLSS, FSR and XeSS do not beat a plain stretch of aliased 2x input. Motion accuracy is not the limit.
  2. (Feasibility looked at 2026-09-29, from public sources only; nothing built or timed here.) Two routes. (a) Shader CNNs in the style of Anime4K (MIT, thin
     densely connected networks of a few thousand parameters, designed to run at 4K as pixel shaders; but trained for anime, so for photorealistic games a
     model would have to be trained and its training data licensed) or FSRCNNX. (b) An ONNX Runtime with the DirectML provider (about 12 MB of redistributable
     NuGet package; DirectML itself is in maintenance mode) running a compact model such as Real-ESRGAN's SRVGGNetCompact: heavier, with real risk of not fitting a
     frame budget at 4K on top of Lossless Scaling's own work. Either is a project of its own with a training/licensing question first, not an afternoon.
     A small neural single-image super-resolution model (DirectML or an ONNX runtime; licence and size to check), as the moving picture instead of a resample: detail from learning, not from frames.
  3. Make the temporal upscalers get jitter: not possible (frames arrive rendered), unless a game is run at a slightly different size each frame (no).
- (Done) A no-reference score for 1:1 (`nr_sreval`: flicker and detail against the game's own frames). (Done) FSR's slow-pan drift: gone with the lean, the three are tied on Silent Hill f.
- Per-backend lean thresholds (one value for all three today): not needed, the three behave alike.
- Profiles per game (check what exists first) and an A/B split preset in the panel for newcomers.

## Motion estimate / frame generation (frame generation stays out of releases: NR_FRAMEGEN=OFF)

- The estimate is not limited by the pyramid's reach nor the block cost (docs/lsfg-vk-study.md). Next: occlusion handling (what comes
  into view), a confidence per level (LSFG's beta), a synthesis pass of our own to compare with FSR 3.1's.
- Adaptive frame generation (the owner uses LSFG's meanwhile): a scheduler over one generator; framegen11.cpp already computes the times.
- Drawing more frames than the refresh rate / the 60 Hz TV limit (owner, 2026-09-27).
- Depth from motion; ReShade depth only as an optional extra (not for online games).

## Research to come back to

- **From the Magpie dissection (docs/magpie-dissection.md, 2026-09-30), in order**: (1) scene-cut reset: measured, not needed (the lean, DLSS's bias mask and NR's luma check already give a clean first frame after a cut); (2) a frame trace (per-frame
  timeline ring, CSV export, `tools/analyze_frame_trace.py`) for pacing questions such as the player's report; (3) a "what is wrong" card in the panel from the existing numbers; then research needing the owner's yes first:
  a user-supplied NVIDIA Video Effects runtime for single-frame VSR (issue #8), XeSS frame generation live (issue #9), the model on a second graphics card (cross-adapter heaps; nobody does it; the plumbing is proven with `nr_xadapter`, design and pitfalls in docs/dual-gpu.md, needs two real cards to measure).

- **Very long term (owner, 2026-09-30): spin this off, the way the Magpie fork is**: a standalone capture-and-scale app rather than an addon inside Lossless Scaling. No plan; keep the engine code (upscaler passes, estimator, NR engine) separable from the host glue. Their code is GPL-3.0: ours would be written from the research (docs/magpie-fork-study.md).

- lsfg-vk (CC BY-NC-ND: study only): the owner wants to discuss what "using code" means, including loading Lossless Scaling's shaders
  from the user's own install at run time (docs/lsfg-vk-study.md).
- The DLSS 5 on Radeon mod (closed, no redistribution): only if an all-in-one or optimised build appears, or new techniques.
