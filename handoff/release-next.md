# The next release: what is in it, what to test first, what to do on release day (written 2026-10-09)

State: everything below is on local `main` (the branch `claude/infallible-diffie-3f06ab` is the same commits); nothing is pushed or released. The version in `manager/CMakeLists.txt`,
`manager/sdk/include/eam/version.h`, the five `addon.json` files and the README is still 0.9.38: the number is the owner's call and he said (2026-10-09) he wants it to be **"1 alpha"**: the version strings are numeric only (`ParseVersion` in manager/src/update/update_check.cpp reads major.minor.patch and builds the zip name `LSAddonManager-<x.y.z>-x64.zip` from it), so use the number 1.0.0 everywhere (tools/ci.ps1 checks they agree) and say "alpha" in the release title / notes (a GitHub pre-release flag would hide it from the updater if it asks for the latest release only: check `kReleasesUrl` / the endpoint first, and decide whether alpha testers should be offered it).

## What it contains (CHANGELOG.md "Unreleased" has the full wording)

- Updates: **Close it and install** (Lossless Scaling is asked to close, Setup updates and restarts it); checks at start-up and daily, never while a game is being scaled; an **Update available** badge in the header; the Setup is hashed again before it runs.
- The addons notice a **repeating capture source** (a 30 fps game in a 60/120 Hz capture feed) and say so; docs/capture-card-sources.md, FAQ entry.
- **Lighter upscaling of generated frames when the card is full** (test, off by default), after the study in handoff/BACKLOG.md (issue #19).
- Safer addon loading: addon.json paths stay in the addon's folder, a settings file from elsewhere keeps this PC's security level, the event bus drains before unloading, dispatch callbacks cannot deadlock the render thread, resource lookups wait at most 50 ms.
- Fixes from two independent reviews: config.json with a wrong-typed window size no longer ends the process at every start, an unreadable config is not replaced, log rotation keeps the old log, installing the Lossless Scaling folder as an addon no longer copies it into itself, the config editor refuses NUL bytes, Lossless Scaling's views are put back after a fault in the addon, the addons report their real version.
- Tools: deploy.ps1 (distinct backups, stops on a failed copy), package.ps1 / run_addon_tests.ps1 (stop on a failed build), video_pacing.py, tap_test bench, shell_events.ps1.

## Checked here (no human eyes needed)

- `tools/ci.ps1`: all CI tests pass from a clean build (the CI's nr_diagtest links the cadence detector).
- `tools/run_addon_tests.ps1 -All` suites: core, features, sample, update, gui, runtimes, installer, Setup exe, NR requirements/settings/auto/diag/compose/trace/recorder/tap/HDR: pass.
- The Neural Rendering host matrix: all 61 scenarios pass, 3 at a time on an idle machine (593 s). Run alone, with another build or game on the GPU, scenarios fail with "NR slower than watchdog" (the model takes 400+ ms): that is the machine, rerun with `--jobs 1`.
- `tools/package.ps1 -SkipBuild`: all five addons in the zip (DLSS5NR01, DLSS4DLAA, FSR3UPSC, VSRUPSC, XESSUPSC), 48 files, Setup exe checked byte for byte (a dev zip is in dist\LSAddonManager-0.9.38-dev-f9391d5-x64.zip; the released 0.9.38 zip on GitHub is untouched).

## Test by eye first (deploy: close Lossless Scaling and the game, then `powershell -File E:\echo-addon-manager\tools\deploy.ps1 -What all -LsDir 'D:\Utilities\Lossless Scaling'`)

1. **Start-up and a normal game** (WoW, frame generation x2, Neural Rendering or the DLSS upscaler on, 5 minutes with a menu and a still scene). Pass: `logs\<addon>.log` has "source cadence: watching the captured pictures for repeats" and never "every picture ... comes N times"; scaling starts with no hitch; frame times in the panel as before.
2. **A repeating source**: a 30 fps video in mpv in a window, scaled with frame generation x2, 10-20 s. Pass: the log says "comes 4 times in a row" (120 Hz screen; 2 for 60 Hz) with a capture rate in it, and the panel's "What stands out" shows the note. If nothing appears, say so: Lossless Scaling may only get new frames when the window changes, and then a capture card is the test.
3. **The update offer**: with Setup (or the real update check) offering a version, **Close it and install**. Watch what Lossless Scaling does with the close request (exit, or hide to the tray?). The offer says what to do next if it stays.
4. **The badge**: an update available, press *Not now*: the badge stays in the header and reopens the offer.
5. **The lighter option when the card is full** (upscaler panel, "...when the card is full (test)", a GPU-bound game with frame generation): the log says when it goes on and off; compare the picture with it off, in fast motion especially (the offline loss there is 1.5-1.7 dB).
6. **Closing a scaled game by its X** a few times; if the taskbar or Explorer misbehaves afterwards, run `tools\shell_events.ps1` and send its output with the time.

## Test games (the owner's choice, 2026-10-09: small ones, not the huge titles)

SUPERHOT (still frames: repeat skip, cadence note), Tomb Raider 2013 (a character with hair turning against scenery: the generator's hard case), DOOM 2016 (fastest camera, static HUD over motion; motion blur / chromatic aberration / grain off), Saints Row 2 from GOG (natively 30 fps: leave GOG's cap on; the capture-card case, the log should say "comes 2 times" at 60 Hz), WoW for lighting. Any game becomes a 30 fps source with an NVIDIA Control Panel or RTSS cap. Windowed or borderless.

## Frame generation of our own: the live look (a prototype; NOT in releases; its own test build)

Build and deploy (the normal build, `addons\DLSS5NR01\build`, has it compiled out, as releases do): `powershell -File tools\build_framegen_test.ps1`, then close Lossless Scaling and the game and `powershell -File tools\deploy.ps1 -What echofg -BuildDir build_fg -LsDir '<folder>'` (EchoFG is an addon of its own since 2026-10-11: `ECHOFG`, "EchoFG frame generation (prototype)" in the manager's list; it needs no upscaler addon, only NIS scaling). Lossless Scaling's own frame generation OFF. Without a GPU needed for Lossless Scaling first: `addons\DLSS5NR01\build_fg\Release\nr_fgpresent.exe mult=4` (the presenter on a real swap chain; prints FG PRESENT TEST PASSED).

1. The EchoFG addon's panel: "Use EchoFG frame generation" on, "Makes the frames": EchoFG (our own), x2. Pass: the panel line says EchoFG; the picture looks like FSR's x2 (it scores the same offline) and, in a turn, the character is not torn; the log (`logs\ECHOFG.log`, every 10 s; `python tools/dev.py logs` condenses it) lists real / made frames, the GPU ms and what the display did with the presents.
2. A/B against FSR's own (checkbox off) and against Lossless Scaling's LSFG x2 (frame generation of ours off, LS's on): the owner's eyes are the judge. Things to look at: the HUD edges over moving background, thin moving lines, the character against sky and leaves in a turn, foliage speckle at edges (the known flaw), ghosting in a sudden stop.
3. x3 and x4 ("Frames for each real frame"): even motion, no stutter between the real frame and the made ones; the cost shown in the panel (about 1.2 ms of GPU for each frame more at 2560x1440).
4. HDR (Windows HDR on, scRGB or HDR10 frames): same picture quality, no banding or blown highlights in the made frames (offline: scRGB scored the same as SDR; HDR10 PQ is the same code, untried).
5. Switch things while it runs (the multiplier, own synthesis on/off, the game's window size): no freeze, no black frame (offline: nr_fgpresent switch=1 and resize=1 pass).

## Release day (the procedure that has worked; memory: release-state, verify-package-contents)

1. Bump the version everywhere; CHANGELOG heading "## Unreleased" -> the version and date; README "New in".
2. Commit specific paths (never `git add -A .`), merge into main, push, `gh run watch` the CI.
3. `powershell tools/package.ps1` in the main checkout (it stops if an addon is not built; the log must list all five addons; the zip is about 172 MB).
4. `gh release create vX --target main --notes-file <notes>`; then the open issues: post the drafts in handoff/issue-reply-drafts.md that still apply.
5. Not in the changelog or notes by the owner's rule: the NVIDIA Video Effects SDK subset that VSR ships with for testing. Before 1.0 it goes (tools/package.ps1 VSR Extra files), with a download option; legal review open (NOTICE.md).
