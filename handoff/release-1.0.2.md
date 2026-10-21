# Release 1.0.2: Video Super Resolution out of prototype (2026-10-20, the same day as 1.0.1)

The owner's call: "lets get it out of prototype. its for movies, images, stuff like that. lets optimise and release". What was done:

- `src/vsr/vsr_addon.cpp`: the motion gate is off by default (VSR on the whole picture, the right thing for a film or an image); a **repeat check** on the card (`kCompareHlsl`: a count of pixels that differ by more than `repeatTolerance` levels from `texRun`, the picture VSR last worked on; read back at once, limit a twenty-thousandth of the pixels) skips VSR for a picture that has not changed and puts out its last result (`outValid`); status line counts `pictures worked on` and `repeats put out again`; the name and texts no longer say prototype; version string through `tools/bump_version.py`.
- Host scenarios: `vsr_repeat` (a doubled source: repeats recognised), `vsr_norepeat` (the check off), `vsr_nogate` (a moving picture is never taken for a repeat); `vsr_move` and `vsr_fade` ask for `motionGate=1` now that it is off by default.
- Docs: `addons/DLSS5NR01/docs/vsr.md` rewritten, README (its own section; EchoFG stays the only prototype), FAQ, CHANGELOG, About tab, manifest.
- Owner: "vsr licensing is not a concern" (NVIDIA's SDK files stay in the package with their licence PDFs; no review item).
- Open (board V1): a live look with a 24 fps film on a 60 Hz screen, 30 fps on 120 Hz, a still image, a paused video, HDR films (VSR takes an 8-bit view); cost of the repeat path at 4K measured only in the host (0.4 to 0.5 ms).
