# Release 1.0.1: what is in it, what to test, what to do on release day (prepared 2026-10-20; not tagged, not released)

State: the version is 1.0.1 everywhere (`tools/bump_version.py 1.0.1 --apply`), CHANGELOG.md has a "1.0.1 (2026-10-20)" section, the README's "New in" paragraph and ROADMAP.md's status line say 1.0.1.
Nothing is tagged. The tag, the zip and the release are the owner's. The full suite (`tools\ci.ps1`, the host matrix) has NOT been run for this number: it runs once, on release day.

## What it contains (CHANGELOG.md has the full wording)

- **Issue #22 (heavy upscalers under frame generation):** "Lighter upscaling of generated frames when the card is full" is on by default and uses a 95 % limit when the manager's GPU limit is off. The upscaler runs on every picture shown, generated ones included; this gives the generated ones the cheap picture while the card is full.
- **Shader cache:** compiled shaders kept under `%LOCALAPPDATA%\LSAddonManager\shader-cache` (about half a megabyte, files unused for 90 days removed, `NR_SHADER_CACHE=0` turns it off). Engine start-up at 4K went from about 7 s to 0.4 s.
- **Issue #21:** the Ctrl+Shift+F9 comparison label has a second line with the runtime's version.
- **Icons:** the recorder, GPU limit, VSR and EchoFG icons.
- EchoFG test-build work (not in the release zip): the motion estimated at half size on frames over 2560 wide, the picture-at-large correction, marks.

## Checked here

- `tools\run_addon_tests.ps1`: all addon suites pass (presenter x2/x4/x8, HDR, EchoFG switched on, upscaler HDR, requirements, settings, auto quality, diagnosis, compose, trace, recorder, tap).
- CI on `main` was green for every push of the day up to the shader cache; the last pushes were still running when this was written.
- `fsr_runtime_switch` (board S7) passed 3 of 3 alone on driver 617.42.

## Test by eye first (deploy: close Lossless Scaling and the game; `pwsh -ExecutionPolicy Bypass -File tools\deploy.ps1 -What all -UnlockWip` with LS_DIR set)

1. **The #22 case**: a GPU-bound game at 3440x1440 or 4K, frame generation Adaptive 120, the DLSS upscaler (preset Auto), once with "Lighter upscaling of generated frames when the card is full" ticked and once unticked. Pass: the log says "the card is at N % (limit 95 %): the generated frames get the lighter upscaling", and the frame rate is higher with it ticked; the picture in motion looks the same to you. (`logs\DLSS5NR01.log`; the FSR and XeSS addons log to their own.)
2. **Start-up**: close the game, start it again: the addons' logs show the compile steps as 0 ms (or no compile lines) the second time; nothing odd the first time (the folder `%LOCALAPPDATA%\LSAddonManager\shader-cache` fills with `.cso` files).
3. **The label**: Ctrl+Shift+F9 steps through the upscalers; each label has "RUNTIME x.y.z" under its name.
4. **The Features tab** and the addon list: the new icons look right at 100 % and 150 % scaling.

## Release day

1. `pwsh -ExecutionPolicy Bypass -File tools\ci.ps1` (full), then the host matrix `python tools\run_hosttest_matrix.py --jobs 1` on an idle machine.
2. `pwsh -ExecutionPolicy Bypass -File tools\package.ps1`, check the zip's SHA-256, then the owner tags `v1.0.1` and publishes.
3. Reply on issue #22 that the fix is released; close #21 if the label is enough.
