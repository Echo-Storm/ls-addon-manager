# 1.0: the one-hour test and release day (prepared 2026-10-20; nothing here is done until the owner does it; no tag, no release was made)

State: `main` is green on GitHub (last full runs 2026-10-20), the version is still 0.10.0 everywhere, `python tools/bump_version.py 1.0.0` was tried on a copy of the tree and
reverted (the dry run lists 11 files; with `--apply` the local CI was run on the bumped tree, result in "Checked before the hour" below). Addon API 1.3.0 is new in this release
(IHost calls appended: recorder and GPU limit; the 1.0 interface is only appended to).

## What 1.0 contains (CHANGELOG.md "Unreleased" has the wording)

Model preset for Neural Rendering (Newer / Older; the 310.9.16 model file is on the list of builds seen working) · Recording is one feature of the manager (Features tab) with sounds, a
note beside each file, Compact / Exact, a key F1-F12, keep-going-after-the-key, and works with no addon on · GPU limit is one feature of the manager (80-100 %, a slider) · the addons'
own recorder and limit are gone · update-check fix · tool fixes. Not in it, on purpose: EchoFG (WIP, off in every release), the Video Super Resolution prototype (WIP, greyed).

## The hour (suggested order; times are budgets)

**0:00 - 0:10 Before touching anything.** Close Lossless Scaling and every game. `git status` clean on `main`, `git pull`. Note the model file's hash is the one on the list (the manager's
Requirements row says so). Back up `D:\Utilities\Lossless Scaling` (deploy.ps1 does it; or copy the folder).

**The full test suite is NOT run in the hour.** `tools\ci.ps1` and the host matrix (`run_hosttest_matrix.py --jobs 3`, about 10 minutes, needs an idle machine) run **once, on the release itself** (release day step 2), not before and not again during the test hour. `tools\ci.ps1` was run once on the bumped tree on 2026-10-20 (below).

**0:10 - 0:15 Bump and package (a dev build to test, not a release).**
`python tools\bump_version.py 1.0.0 --apply`, build (`python tools\dev.py build`), `powershell -File tools\deploy.ps1 -What all -LsDir 'D:\Utilities\Lossless Scaling'`, `powershell -File tools\package.ps1`
(stops if an addon is not built; the log must list the five shipped addons: DLSS5NR01, DLSS4DLAA, FSR3UPSC, XESSUPSC and the manager; no ECHOFG, no VSR, **no nvngx_dlssnr.dll**).

**0:15 - 0:55 Live, with Lossless Scaling and WoW (or any game), in this order.** Each: what to do, what passes. Logs are in `D:\Utilities\Lossless Scaling\logs` (`LSAddonManager.log`, `DLSS5NR01.log`).
1. *Start-up (3 min).* Start LS, open the manager. Pass: header says 1.0.0, no error banner, Features tab lists ReShade, Windowed, Recording, GPU limit; Addons tab lists the four addons, all loaded; About says 1.0.0.
2. *Recording, no addon (6 min).* Neural Rendering OFF. Scale WoW with frame generation OFF. Ctrl+Shift+F1 (Recording on, Compact). Pass: a tick the moment you press, a rising pair after a few seconds, a `.lsrec` and a `.lsrec.json` note in the folder; the note names the game and the picture; `python tools\dev.py rec info <file>` shows frames and size (about 1 GB for 10 s at 1440p Compact). Then frame generation ON: same.
3. *Recording options (4 min).* Exact instead of Compact (bigger and slower to save: expected). Keep going after the key = 3 s. A press with nothing to record (LS not scaling) gives the low tone and a log line, not silence.
4. *GPU limit (5 min).* Features, GPU limit on, 85 %. Pass: the status line shows the card's load; with a GPU-bound scene Neural Rendering's Auto quality takes the working size down and returns when the load drops; limit off: back to normal. (Two-card machine: the load must be that of the card LS runs on.)
5. *Model preset (6 min).* Neural Rendering on, Newer, then Older (Model passes). Pass: the picture changes between them, the model time in the panel is about 40 % lower on Newer, no error in the log; Ctrl+Shift+F7 before/after still works.
6. *Upscalers (4 min).* DLSS Upscaler, FSR Upscaler, XeSS Upscaler one after the other (conflicts are named when two are on). Pass: picture, no freeze, panels open.
7. *ReShade and Windowed features (3 min).* One look each as in their Features text. Pass: on and off without a restart of the game.
8. *Settings and safety (3 min).* Close the manager window and reopen from the tray; change a slider, restart LS: the setting stayed. Close the game by its X twice: no stuck taskbar.

**0:55 - 1:00 Decide.** Everything above passed: go. Anything failed: write it on the board (`handoff/board.json`) and decide per item whether it blocks.

## Release day (only when the owner says; these are the steps that worked before, see also release-next.md)

1. `python tools\bump_version.py 1.0.0 --apply` (if not already), write by hand: README "New in 1.0.0" (a short block, below), ROADMAP.md status (below), CHANGELOG.md (the bump moved "Unreleased" under 1.0.0).
2. Commit exactly those paths (never `git add -A .`), push, `gh run watch` the CI until green; then, the once-only full run: `pwsh tools\ci.ps1` (run the Setup exe step with `-ExecutionPolicy Bypass` if this machine's policy blocks it) and `python toolsun_hosttest_matrix.py --jobs 3` on an idle machine; all must pass before the zip is made.
3. `powershell tools\package.ps1` in the main checkout; open the zip: five addons, the Setup exe, no model file, licences present (about 172 MB). Check the Setup exe against the build (the script does).
4. `gh release create v1.0.0 --target main --notes-file <notes below> <the zip>`. Then post the reply drafts for issues #20 and #21 (`handoff/issue-reply-drafts.md`).
5. Not in the notes by the owner's rule: the NVIDIA Video Effects SDK subset the VSR prototype uses for testing.

## Rollback

`deploy.ps1` keeps the previous files in `backups\` (distinct per deploy); put them back, or reinstall the 0.10.0 zip over it (the manager's own settings and the addons' settings stay). The model file was replaced on 2026-10-19: the 310.8 file is in `backups\nvngx_dlssnr.310.8-4b8d19bc.20261019-013639.dll`.

## Text to paste (drafts; the owner's words win)

**README, "New in 1.0.0" (and change the header line's "on the way to 1.0" to the stable wording):**
> **New in 1.0.0: one recorder and one GPU limit for everything, and a choice of model.** Recording is now a feature of the manager (Features tab): press a key, hear it, get a clip and a note beside it, with or without any addon on and with or without frame generation. The GPU limit is a slider there too. Neural Rendering has a Model preset (Newer is lighter, Older is the original look). The addon API is 1.3 (appended calls only; addons built for 1.0-1.2 keep working).

**ROADMAP.md, status line:** "Status on <date>, at version 1.0.0: the promises the page lists (the addon API, the settings file, safety) are stable; what is not in 1.0 is listed under 'After 1.0'."
(Check the table in ROADMAP.md row by row first: it was last written at 0.10.0; EchoFG and VSR stay "work in progress".)

**Release notes:**
> **1.0.0: one recorder, one GPU limit, a model choice, and a stable addon interface.**
> - **Recording is a feature of the manager.** Features tab, Recording: a key (F1-F12 with Ctrl+Shift), a tick when you press and a rising pair when it is saved, a note beside each file, Compact (8-bit, about a sixth of the size) or Exact, keep going after the key. It finds Lossless Scaling's frames itself, so no addon has to be on, and with frame generation off it records what the scaling pass reads.
> - **GPU limit is a feature of the manager.** One slider (80-100 %) for all the addons: Neural Rendering's Auto quality and the upscalers' lighter mode follow it. Works with NVIDIA (NVML) and other cards (Windows counters).
> - **Neural Rendering: Model preset (Newer / Older).** With NVIDIA's runtime 310.9.16 the default is the newer, smaller model; Older is the previous look. Your model file is still your own (not included, not downloaded).
> - **Addon API 1.3** (calls appended: `RecorderFlags`, `RecorderOffer`, `RecorderSave`, `GpuLimitPercent`, `GpuLoadPercent`, the event `EAM_EVENT_RECORDING_SAVED`).
> - **Fixes:** a second "Check now" straight after a check was ignored; a crash path in the host's graphics-card naming; recorder shutdown while a frame is being copied.
>
> If you have an earlier version, this replaces it. Download `LSAddonManager-1.0.0-x64.zip`, unzip, run `LSAddonManagerSetup.exe`. SHA-256 of the zip: <fill in after packaging>.

## Checked before the hour

- 2026-10-20, `pwsh tools\ci.ps1` on the tree with the 1.0.0 bump applied (then reverted): version check, manager and its tests (install, handling, update check, sample addon), installer core and payload tests, the addons' offline tests: all passed. The one failing step, "Setup exe, silent mode", failed only because this PC's PowerShell policy refuses scripts ("running scripts is disabled"); run with `-ExecutionPolicy Bypass` the same test passes (SETUP EXE TEST PASSED, 0 failed).
- The host matrix was not run (release only).
- GitHub: every completed run of 2026-10-20 is green; the last two (docs and tools only) were still running at the time of writing.
