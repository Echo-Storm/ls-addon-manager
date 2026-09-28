# Backlog

Everything worth doing that is not done yet, so nothing is lost between sessions. Newest context first within each group. Update it when an
item is done (move it to the changelog) or when a new one comes up. Dates are when the item was noted.

## Released

- 0.9.15 (2026-09-28, v0.9.15 at 6d896fe): the HDR neon fix (issue #3, likely #1). Issues #1, #2, #3 answered on GitHub, left open for the reporters to confirm.
- 0.9.14 (2026-09-28, v0.9.14 at ae232c0): see the changelog. Confirmed live: the corner square in the upscaler addons; NR smoothing at 0.6 helps.

## Bug sweep (planned Tuesday 2026-09-29 morning)

- `run_hosttest_matrix.py --only scaler_bgra` fails ("NIS KEPT": the DLSS engine starts in about 9.5 s and the test ends before a frame
  is upscaled); also fails on the code before 2026-09-28's changes, so it predates them. `scaler_not_nvidia` shows NIS KEPT too. The
  baseline (`base`) fails when run alone with `--only` (known: baseline checks need their scenarios in the list).
- **One recorder for all addons** (owner, 2026-09-28): each addon has its own "Keep the last few seconds" setting, so the owner had it on in the FSR Upscaler while running Neural Rendering with the DLSS Upscaler, and nothing was saved. The save key now says which addon's recorder is off; the fix is one shared setting (or the manager's own).
- The corner square: the output swap chain is only learned while the upscaled picture is shown, so after a restart that begins on
  "original", the first toggle shows no square.
- Neural Rendering's changes in 0.9.13 (temporal smoothing clamp, NGX user count) were never tried live before release; the NGX count
  was seen working live on 2026-09-28 ("NGX left running (another addon still uses it)").

## Neural Rendering

- **Flickers "like crazy" while moving: the weakest link now** (owner, 2026-09-28). First try: smoothing keeps its history where the frame matches along the motion, on by default at 0.6 (saved settings keep 0). Not confirmed as the cause. If it does not help: record with NR on, build `nr_nreval` (the change's stability frame to frame), check the compose's one-frame-old result moved by its motion (a.offset) and the half-size model input.
- Live with smoothing 0.6 (2026-09-28): "doing quite well", a weird flicker left on landscapes. The NR recorder was off (its own "Keep the last few seconds" box, off by default), so nothing was saved: next time a screen video plus an NR recording.
- Earlier: the owner wants it improved (look and cost).
- Cost: in World of Warcraft at 4K the model takes about 8.5 ms a frame at half size (1912x1080), over the automatic budget (5 ms), and
  the automatic size is already at its floor (0.50): "auto: model resolution 0.50 -> 0.50". Frames over 20 ms: 54-82 %.
- Ideas: a lower floor for the automatic size (0.4 / 0.33) with the compose's upsampling of the change; run the model every other frame
  and carry its change along the motion (the compose already moves a result by its motion vectors); a quality score for NR offline (an
  `nr_nreval` like `nr_sreval`: the change's stability from frame to frame on a recording).

## Upscalers

- World of Warcraft upscaled 1.5x: DLSS still 1.1 dB under a plain stretch on the coarse score (43.14 vs 44.24) in walking motion.
- FSR's slow-pan drift (about 45.6 against 49.8 on the Silent Hill f pan); a settings sweep with `nr_sreval`.
- A steadiness score that needs no reference picture, for judging 1:1 (DLAA) offline: at 1:1 the reference is the game's own aliased
  frame, so leaning always scores better.
- Per-backend lean thresholds (one value for all three today).
- Profiles per game (check what exists first) and an A/B split preset in the panel for newcomers.

## Motion estimate / frame generation (frame generation stays out of releases: NR_FRAMEGEN=OFF)

- The estimate is not limited by the pyramid's reach nor the block cost (docs/lsfg-vk-study.md). Next: occlusion handling (what comes
  into view), a confidence per level (LSFG's beta), a synthesis pass of our own to compare with FSR 3.1's.
- Adaptive frame generation (the owner uses LSFG's meanwhile): a scheduler over one generator; framegen11.cpp already computes the times.
- Drawing more frames than the refresh rate / the 60 Hz TV limit (owner, 2026-09-27).
- Depth from motion; ReShade depth only as an optional extra (not for online games).

## Research to come back to

- lsfg-vk (CC BY-NC-ND: study only): the owner wants to discuss what "using code" means, including loading Lossless Scaling's shaders
  from the user's own install at run time (docs/lsfg-vk-study.md).
- The DLSS 5 on Radeon mod (closed, no redistribution): only if an all-in-one or optimised build appears, or new techniques.
