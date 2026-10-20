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
