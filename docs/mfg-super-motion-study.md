# MFG Super Motion: what its public files tell us about how it works (ideas only)

Written 2026-10-20. Source: NODIX-TECH/MFG-SUPER-MOTION on GitHub (the F# / Avalonia front end, source-available under PolyForm Strict; the engine is closed and is **not** in the repository), its README, its English strings and its release list. Nothing from that project is copied, linked or shipped here; this is what we learnt from reading the public text, to be used as inspiration for our own work (EchoFG, the Neural Rendering addon, the manager).

Facts below are read from `Infrastructure/MotionEngine.fs`, `Localization/Translations/en.json`, `THIRD-PARTY-NOTICES.txt`. Lines marked *(inference)* are ours.

## How the program is built

- The program is a **front end plus a separate native engine process**. The front end writes a start file (`key=value` lines), starts the engine (`--config start.cfg`), then sends changed controls as `key=value` lines on the engine's standard input and reads `@state`, `@fps`, `@source`, `@mfg`, `@vrr`, `@aml`, `@error` lines from its output. `stop` on stdin (or a named event `Local\SuperMotionEngineStop`) ends it, and "it puts the game's window back as it was". This is a clean, crash-isolated split: a fault in the engine never takes the UI with it, and the UI can restart it.
- **The engine file is an executable named `nvngx.dll`.** The comment says "NGX checks its caller's file name". So the engine calls NVIDIA's NGX from a standalone process and has to carry the name NGX expects from a caller. *(inference)* This is how it reaches NVIDIA's own frame generation without being a game.
- It lists cards with `--cards` (`@card <n> <DLSS MFG 1|0> <MB> <name>`, `@xess <n> <1|0>`): whether **DLSS multi frame generation** can run on each card (an RTX card), whether **XeSS frame generation** can (Intel's library answers). The user picks a card ("Preferred GPU").
- Three **generators**: 0 = DLSS MFG, 1 = XeSS FG, 2 = "MSM FG" (their own, "works on any graphics card and is the lightest"; "SMFG · Fast" and "SMFG" differ in how carefully the motion is looked for).
- **Motion source** per generator: 2 = the generator's own, 0 = NVIDIA Optical Flow, 1 = a GPU estimate. *(inference)* The vendor generators need motion vectors (and depth) that a game normally supplies; for an arbitrary window the engine has none, so it makes the motion field from the two captured pictures (NVIDIA's Optical Flow hardware, or its own GPU search) and hands that to the vendor generator. That is the one idea that matters most for us.
- Multipliers x2 to x6 ("mode + 2").
- Capture: `dxgi` or `wgc` (Windows Graphics Capture), output placement `borderless | scaled | window`, shown on top of the game; Spout output for OBS (a recorder otherwise sees the game without the made frames).
- Pacing: queue target (lower = less delay, higher = smoother), low latency, frame limit, "limit to refresh rate", VRR (and a check that VRR actually reaches the screen: "G-SYNC for windowed mode"), sync mode (off / default / vsync), HDR toggle.
- Motion options: **Game type** (tells the engine what to expect; "Custom" opens **Flow**, the share of the picture the motion is worked out at: lower is lighter), **Fast motion guard**, **Keep still pixels** (HUD, crosshair and text stay sharp and steady: "the HUD ... as sharp and steady as the game drew them"), MSM search depth.
- **Adaptive Motion Learning (AML)**, per game, stored as a small text file (`Learned\<game>.aml`, lines `game=`, `percent=`, `learned=`, `updated=`, plus a `.hud` file beside it): "learns, for each game, how your mouse, keys and gamepad move the picture, how late the game follows, and where its HUD is". A short guided study the first time ("stand still, move the camera left and right", "walk forward and back", "drive a car"), "costs a little performance, gets better when done".

## What we can take from it (our own way, not their code)

1. **HUD / still-pixel map learnt per game.** Their "where its HUD is" is the same problem as our E1/E2 (HUD edges over moving background; blocky cells on periodic textures). We already detect static regions per frame; a *persistent* per-game mask (pixels that stayed identical over many seconds across camera motion = HUD) would be steadier than a per-frame guess. Cheap to try offline on the recordings: accumulate "unchanged while the neighbourhood moved" over a clip and compare against the oracle.
2. **Input-to-motion lag as a learnt per-game number.** "How late the game follows" is the time between input and picture change; it lets the generator extrapolate with the right amount of latency. We have the frame trace and the cadence probe; the input side is new (mouse / key / pad events from the manager, which already has hooks and a hotkey thread).
3. **Vendor frame generation fed our own motion field.** If NGX's frame generation (DLSS-G) can be driven with a motion field and no depth from a standalone caller, the work on our side is the motion estimate, which we have (EchoFG's flow). This is also exactly what the board card I9 ("XeSS FG / DLSS FG request") asks for. It must be *tested*, not assumed: whether DLSS-G accepts zero/synthetic depth, whether it needs the NGX caller-name check satisfied, and what the driver does when it is not a game. NVIDIA's own terms for the DLSS-G / Streamline SDK have to be read before anything of it ships in a release.
4. **Engine-as-a-process.** Ours are in-process addons (right for the hook design), but an out-of-process generator behind the same text protocol would isolate driver faults (our S6 card: a fault in NVIDIA's driver in 1 of 20-60 host runs). Worth a design note, not for 1.0.
5. **VRR reality check** ("VRR asked for and not reaching the screen") and **Spout output for recorders**: the first is a diagnostic we do not have (EchoFG display timing, E3), the second is related to our recorder: a recorder that sees the presented frames. Our manager recorder already offers the presented picture for addons that draw one.
6. **A learning/"study" mode that is visible**: a status line with the percentage and a short guided step is friendlier than a silent calibration.

## What we do not do

- We do not ship, load or copy their engine, DLLs, models or text. The engine is closed and the licence forbids redistribution or derivatives. Everything above is a description of a *method* anyone could try, written from what the public files say.
- We have not run their program. Anything about its internals beyond the public strings is guesswork until tested on our side.

## Experiments, in order of value

1. **DLSS-G feasibility** (card I9): a small standalone test (`nr_*` tool style) that tries to create a frame-generation feature through NGX/Streamline with our flow field and no depth, on the 4070 Ti SUPER, and records exactly what the API says. Pass/fail is information either way.
2. **Persistent HUD mask** on the recordings (offline, `nr_*` tool, scored against the oracle like the other EchoFG steps).
3. **Input lag measurement** in the manager (events with timestamps, correlated with picture change in a recorded clip).

## What the release 1.0.2 files add (looked at 2026-10-20: file lists, readmes, exported names and strings only; nothing run, nothing copied)

The release folder (`NEW FILES/research/`, kept out of git) confirms the inferences above and says more. It is observation of what the files *are* and *say*; no code was disassembled.

- **The engine is NVIDIA's DLSS frame generation, called through NGX Direct3D 12, fed by motion the program makes itself.** Its text lists the NGX parameters it fills (`DLSSG.Backbuffer`, `DLSSG.MVecs`, `DLSSG.Depth`, `DLSSG.MultiFrameCount`, `DLSSG.MultiFrameIndex`, `DLSSG.OutputInterpolated`, ...: the names are in NVIDIA's public SDK headers) and its log says "**experimental flat-depth guides**" and "flat depth": the depth input is a constant plane and the motion vectors are estimated from the two captured pictures. The compatibility readme says it outright: "supplies estimated motion and a constant depth plane. It cannot reproduce engine depth, HUD-free colour or engine motion vectors."
- **Two ways to make the motion**: NVIDIA's optical flow hardware (`NVOFA`, D3D12 path, two-way/bidirectional, a luma plane at a divided size) and an own GPU estimate: compute shaders named `FlowCoarse`, `FlowRefine`, `FlowSpread`, `FlowFinal`, `FlowCheck` (an image pyramid, a coarse-to-fine search around the neighbours' and the lower level's motion, motion stored as 12-bit signed fixed point at 1/8 texel, a **forward-backward consistency check** "does the way back come home within a texel and a half", a median of the motion around). Our EchoFG flow works in this family already; the two things to compare are the consistency check and the median step.
- **"Still pixels"** are four small passes (`still-calm`, `still-tiles`, `still-grow`, `still-show`): find where the two real frames are the same while the motion around says things move, grow the marks, and show those pixels as the real frame has them (HUD, crosshair, text). Comment in the shader text: a flat patch matches any motion, so its match says nothing; held single texels of moving grass were being taken for HUD (the speckle around a HUD at a low frame rate). Same idea as the persistent HUD mask in the list above, and the same trap to avoid.
- **A rebuild at the picture's size**: the in-between frames are made at a smaller size and rebuilt at the picture's size from the real frames around them (blocks), then drawn straight onto the back buffer or beside it and copied. A cost lever we have not used: generate at reduced size, rebuild with the full-size real frames as the guide.
- **Multi-frame**: x2 to x6 by `MultiFrameCount` / `MultiFrameIndex`. NVIDIA's runtime only does this natively on RTX 50; for RTX 20/30 and 40 the program lays down **a community compatibility package** (sdli1995/dlssg_for_sm86, MIT for its own source, kernel images in binary only; for the 40 series an "Ada temporal kernel adaptation" derived from OptiScaler forks, one header under GPL-3.0-only). Those are other people's work with their own terms: we would not bundle or adapt them, and anything GPL is out for us. It does mean *our own card (RTX 4070 Ti SUPER, Ada) gets x2 from NVIDIA's runtime without any of that*; x3 and up on Ada is the part that needs the package.
- **Bring-your-own runtime**: it accepts an `nvngx_dlssg.dll` placed by the user if it carries NVIDIA's signature, and falls back to its bundled copy otherwise. That is our rule too (the model is the user's own): the driver on this machine already has `nvngx_dlssg.dll` in its driver store, so a test needs nothing downloaded.
- **NGX checks the caller's file name** and the program also sets the DLSS-G *preset* in the driver's profile for its own executable (through NvAPI's driver settings). Whether a program under another name is accepted, and whether we would be allowed to touch the driver profile, are exactly the open questions for the feasibility test. (It also has switches named like `LF_ARCH_SPOOF` / `LF_ARCH_FORCE` that change what GPU architecture is reported; we would not do that.)
- **Presenting**: its own presenter on top of the game window with a queue target, per-pair "moment" scheduling, a measured desktop-to-screen latency (avg and p95), a relay when the screen is on another card, a retry at a lower multiplier when the runtime refuses ("multiplier x refused 0x...; retrying"), and a fallback to ordinary output when anything fails. The fallback-on-failure habit is worth copying everywhere.
- **XeSS frame generation** is called through Intel's `xefgSwapChain*` API (the library `libxess_fg.dll`), with a small plugin that unlocks more than one in-between frame. Same shape as the NVIDIA path: motion estimated, depth flat.
- **Adaptive Motion Learning, in its own log lines**: it listens to mouse, keys and pad *only* for how far they go and which are held (the log says so, "nothing else"); fits how late the picture follows the hand (ms), how much of the picture's movement the hand explains (a percent), and a gain; learns which cells of the picture stay while it moves (the HUD) from many pairs; and from "nine pairs in ten move the picture N pixels a frame or less" it picks an **Adaptive Flow Scale** (the size the motion is looked for at). Those numbers are cheap to measure for us once the recorder can also write input events.

## What this changes in our plan

- The main question is no longer "can it be done": a program that is not the game can drive DLSS-G with estimated motion and flat depth, and its authors ship it. The question is quality and rules. A flat depth plane makes DLSS-G's occlusion/disocclusion handling weak, which is where their own work (still pixels, rebuild, consistency check) goes.
- **Board card I9 becomes a concrete test** (card S8): a tool that loads the driver's own `nvngx_dlssg.dll`, creates the feature through NGX D3D12 with the public SDK header names, feeds it two recorded frames, our EchoFG motion field and a constant depth, and reports every return code. Run on this card (Ada, x2). Pass or fail is information; no release depends on it.
- Rules before anything ships: NVIDIA's SDK licence on DLSS-G / Streamline use, no driver-profile edits without the user asking, no architecture spoofing, no bundled third-party kernel packages.

### How we would do the probe (the public route)

The NGX parameter names in their engine are NVIDIA's private interface (they are not in the public NGX headers, which only list the feature id `NVSDK_NGX_Feature_FrameGeneration`). We do not build on a private interface learnt from someone else's binary. The documented route is **Streamline's `sl_dlss_g` API** (public headers in NVIDIA's Streamline repository, the same SDK family as the 2.14.16 already on card U4): `slInit`, `slSetD3DDevice`, `slDLSSGSetOptions`, and tagging the depth, motion vectors and HUD-less colour with `slSetTagForFrame`, with our estimated motion and a constant depth in those tags. That needs the Streamline SDK files (a download from NVIDIA's GitHub, with its licence) and a D3D12 swap chain. Next step needs the owner's yes for that download.

### Result of the first probe (2026-10-20, RTX 4070 Ti SUPER, driver 617.42, Windows 11 26300; `tools/dlssg_probe`, built on Streamline 2.14.16)

`dlssg_probe.exe` is a small D3D12 window with a pattern that scrolls a known distance a frame, a constant depth plane, motion vectors we write ourselves (exact, zero or wrong), and Streamline's DLSS-G plugin on its swap chain. Run on this machine, with nothing of anyone else's in it:

- **Streamline refuses a program that gives no application ID**: `Please provide correct application id when calling slInit - NGX based features will be disabled` (application IDs are issued by NVIDIA to registered applications). With `applicationId = 0`, DLSS-G never loads.
- **The documented alternative works**: `engine = eCustom`, `engineVersion = "1.0"` and a `projectId` GUID of our own (Streamline's own preferences allow "applicationId, or engine type and version"). Then `slInit`, `slIsFeatureSupported(DLSS_G)`, `slDLSSGSetOptions` all return `eOk`, DLSS-G's state is `eOk` ("max frames to generate 1": x2 on this Ada card; x3 and up need an RTX 50), the plugin signature check passes (the SDK's own DLLs, signed by NVIDIA).
- **It generates frames**: with Reflex's frame limiter at 60, DLSS-G off gives 61 real frames a second and on gives 31 (the limiter counts what is shown: 2 shown for each real one); unlimited, off 1251 and on 625 real frames a second; the state call reports 2 frames presented for each real one against 1 with it off.
- **The inputs it accepts from a non-game**: depth is a constant plane, the camera constants are a fixed camera, `cameraMotionIncluded = true`, and the motion vectors are our own (a constant pixel offset here). Status stays `eOk`: it does not reject flat depth or estimated motion.
- **Quality, measured** (second round; the eye test was inconclusive on the plain pan). The generated frames are read with **Desktop Duplication** (`grab=FILE.raw`: 240 desktop pictures of a 192 x 192 patch, every one the compositor made; GDI grabs and a screen-copy were not reliable) and scored by `check_grab.py` against the true picture, drawn exactly (the pattern is known): the position it shows (real frame = a multiple of the speed, generated = half way) and the error as 1 - correlation (the desktop picture is brighter than what was drawn, so a brightness-blind score is used; 0 = identical). Results, same scene each, only the motion vectors we hand over differ:

  | scroll speed | motion vectors | real frames' error | generated frames' error | worst generated |
  |---|---|---|---|---|
  | 8 px a frame | exact | 0.0043 | **0.0043** | 0.007 |
  | 8 px a frame | zero | 0.0043 | 0.0046 | 0.008 |
  | 8 px a frame | wrong (opposite way) | 0.0043 | 0.0159 | 0.040 |
  | 40 px a frame | exact | 0.0040 | **0.0043** | 0.006 |
  | 40 px a frame | zero | 0.0039 | 0.0159 | 0.024 |
  | 40 px a frame | wrong (opposite way) | 0.0039 | 0.0334 | 0.127 |

  Read: the generated frames land exactly half way between the real ones in every case (the timing is right); with **exact vectors the generated frames are as clean as real ones** even with a **flat depth plane**; with no vectors DLSS-G's own optical flow rescues a slow pan but not a fast one (error x3.7 at 40 px); with wrong vectors it is worse (x7.8, worst x20). So **DLSS-G really uses the motion we give it, and its output quality follows the quality of that motion**. This is the practical consequence for us: our flow field (EchoFG's, which has a 4-5 dB gap to the oracle on the recordings, board E8) is the lever, and a plain pan is the easy case; the next probe must use a field with several motions and edges (a moving object over a moving background, a static HUD patch).
- **What this means for a product**: DLSS-G through Streamline *owns the swap chain* (it presents the extra frames itself, needs Reflex markers, and the extra frames come out of the proxy swap chain). Lossless Scaling already presents its own output, so this can only be the *final presenter* of an own full-screen window (what MFG Super Motion does: capture the game, draw on top), not something added inside Lossless Scaling's pipeline. That is a different product shape from our addons (a presenter process like EchoFG's `nr_fgpresent`, with LS's scaling as the capture). NVIDIA's terms for shipping Streamline/DLSS-G and the application-ID question (we have none) need an answer before anything goes in a release; for a user's own machine the project GUID route above runs today.

Open: how it behaves with real estimated motion (EchoFG's flow field instead of a constant), with a hudless/UI split, at 4K, and at x3/x4 on Ada (which NVIDIA's runtime does not allow; that is what the community compatibility packages the other program carries are for, and we would not use them).

### The harder probe: real footage, EchoFG's flow estimator as the motion (2026-10-20)

`dlssg_probe.exe clip=FILE.lsrec start=N count=M motion=flow|zero grab=OUT.raw` plays a recording (Compact, cropped to a 1280 x 720 window) as the picture: the even clip frames are the real frames (30 a second), the odd ones are the **truth** for the frame generated between two real ones. The motion handed to DLSS-G is either **EchoFG's flow estimator** (`FlowEstimator`, the addons' own source, compiled into the probe; per-pixel, current -> previous, in pixels) or zero; depth is the same constant plane. The screen is read with Desktop Duplication (every picture the compositor made) and `check_clip.py` scores each *generated* picture by the correlation of its brightness with the true in-between frame, next to what needs no model: the two real frames blended half and half (and either repeated). The brightness mapping of the screen is learnt from the real frames and applied to the baselines too; the measurement's own ceiling is 0.988-0.994 (a real frame matches itself that well).

| clip (WoW, 2560x1440 cropped) | motion | DLSS-G generated (mean) | the two frames blended (same pictures) | difference |
|---|---|---|---|---|
| 23-16-58, camera moving | EchoFG flow | 0.826 | 0.758 | **+0.068** |
| 23-16-58 | zero | 0.721 | 0.736 | -0.015 |
| 23-18-17, sunset, high contrast | EchoFG flow | 0.788 | 0.772 | **+0.016** |
| 23-18-17 | zero | 0.813 | 0.846 | -0.032 |
| 23-16-37, building, small steps | EchoFG flow | 0.909 | 0.906 | +0.003 |
| 23-16-37 | zero | 0.911 | 0.906 | +0.005 |

(Each run captures a different set of about 40 generated pictures, so only the difference to the blend of the *same* pictures is comparable; one run per cell.)

Read: **our flow beats no motion by 0.05-0.08 where the picture moves** (the camera-moving and sunset clips) and equals it where almost nothing moves; with no motion DLSS-G does worse than a plain blend on moving footage. But **DLSS-G with our flow is only a little better than blending the two real frames** (+0.07 at best, +0.003 on a calm clip): it is not a free quality jump. The ceiling is the motion and the flat depth: at object edges, and where the picture's own motion is non-rigid, the flow is the weak part (the 4-5 dB gap to the oracle, board E8). What is still to be measured is the same truth frames scored for **EchoFG's own synthesis** and for FSR's, with the same correlation (the existing battery, `dev.py gen`, scores those in dB against the same kind of truth; the two scales are not comparable until one scorer reads both), which decides whether handing our flow to NVIDIA's generator is worth more than finishing our own.

Not done: exact (oracle) vectors on footage (the game's engine vectors are unavailable); a hudless/UI split; 4K; x3/x4; the same clips at the true 60 Hz of the source (the probe plays them at 30 real a second).

### Head to head: DLSS-G, FSR 3.1 and EchoFG on the same footage, one scorer (2026-10-20)

`nr_fgeval` (the project's offline generator test) now takes `dumpraw=FILE` and writes, for each frame it rebuilds, the rebuilt picture, the true frame and the two real frames around it (the middle 1280 x 720); `tools/dlssg_probe/score_dump.py` scores them with the **same correlation score** as the probe's screen captures (`check_clip.py`). Same three segments (clip frames, 59 rebuilt frames each), all three generators get **this project's flow estimator and a flat depth**: FSR 3.1 frame generation (`direct=1`), EchoFG's own synthesis (`own=1 direct=1`), and NVIDIA's DLSS-G through Streamline (the probe, about 40 captured pictures each). Gain over the plain blend of the two real frames, on the same pictures (blend itself 0.71 / 0.75 / 0.89):

| segment | DLSS-G + our flow | FSR 3.1 | EchoFG own synthesis |
|---|---|---|---|
| 23-16-58 (camera moving) | +0.068 | +0.090 | **+0.096** |
| 23-18-17 (sunset, high contrast) | +0.016 | +0.098 | **+0.111** |
| 23-16-37 (building, small steps) | +0.003 | +0.037 | **+0.039** |

(The same pictures in dB, from `nr_fgeval`'s own scorer, gain over the blend: FSR +0.62 / +0.62 / +1.95 dB, EchoFG +0.98 / +1.20 / +2.14 dB.)

Read: **EchoFG's synthesis is at least as good as FSR's and clearly better than DLSS-G driven by our flow and a flat depth** on all three clips; DLSS-G with this input is barely better than a blend on the harder two. Cautions: the DLSS-G numbers come through a screen capture (its ceiling is 0.988-0.994, a few thousandths, nowhere near the 0.03-0.08 gaps), about 40 pictures a cell, one run each; its generated frames are placed in time by its own pacing; and DLSS-G is built for engine depth and engine vectors, so the flat-depth input is its worst case, while FSR and EchoFG take it better. What this does **not** say: how DLSS-G does with real engine data (not available to us), or what a better flow would give each (all three share the flow estimator; board E8).

**Decision input for 1.0 and after**: nothing here argues for adding a DLSS-G generator to the products. The ideas worth keeping from the MFG Super Motion study are on the *motion* side (a persistent per-game HUD mask, an input-lag measurement, a flow consistency check), which help every generator we have. The probe stays in `tools/dlssg_probe` (not built by CI; needs the Streamline SDK, not in the repository).

### Follow-up: NVIDIA's optical flow hardware as the motion source

Tested the same day: docs/optical-flow-study.md. Short answer: on the recordings our own flow estimator beats NVOFA on every clip, and fusing the two adds nothing; NVOFA is only cheaper (0.4 ms on its own engine). Kept as a speed idea (board E14), not a quality one.

### Second look at the engine's own generator

The engine also carries the source text of its own generator (not the vendor-FG path): what it does step by step, where EchoFG stands, and a measured result (a direct draw from two real flow fields matches EchoFG's whole synthesis) are in docs/mfg-super-motion-engine-notes.md.
