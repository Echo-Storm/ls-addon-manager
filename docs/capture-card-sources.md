# Capture-card and duplicated sources (research, 2026-10-08)

Prompted by a Reddit post (r/losslessscaling, u/Hakan76, "30 FPS Console Scaling", Elgato 4K X, a capture app called Nitlink, RDR2 on a PS5). A capture feed of a console is a real use case for
this project (see BACKLOG: the 4090 player who wants GTA VI through an HDMI 2.1 card). This note is what the post claims, what is checked, and what it means for the addons. The post's
numbers and star ratings are one person's impressions; nothing below was measured here yet unless it says so.

## What happens when a 30 fps game is captured at 60 or 120 Hz

The capture card delivers the feed at the signal's rate, so a 30 fps game at 60 Hz arrives as A A B B, at 120 Hz as A A A A B B B B. Lossless Scaling's frame generation (LSFG) makes one
picture between each pair of input frames:

| Source | Output at x2 (positions of the motion) | Result |
|---|---|---|
| 30 capture | A, mid(A,B), B, mid(B,C) ... | even steps; the widest gap to bridge, so the most halo around moving things |
| 60 capture (A A B B) | A, A, A, mid(A,B), B, B, B, mid(B,C) | the motion advances unevenly (3 equal, then a half step): judder; no halo between equal frames |
| 120 capture (A A A A B B B B) | motion changes once in every 4 frames | most stutter; 75 % of the generated frames have nothing to warp |

This is geometry, not a measurement, but it agrees with the post. The rate of the output should also divide the display's refresh (BACKLOG, the 4090 player's logs: 80 to 118 fps on a 120 Hz
screen is uneven; 30 x 4 and 60 x 2 are even).

## What others say

- Set the capture to the game's real rate (30 for a 30 fps game); a 60 Hz feed of 30 fps content makes Lossless Scaling see an erratic rate (Steam discussions of Lossless Scaling,
  https://steamcommunity.com/app/993090/discussions/0/4338735089007356929).
- In emulators the fix is not to present duplicates (PCSX2 "Skip Presenting Duplicate Frames"); some games make duplicates themselves
  (https://steamcommunity.com/app/993090/discussions/0/4418677017728092474).
- Nothing found saying that Lossless Scaling detects and drops duplicate input frames. Treat as absent, unconfirmed.
- "Nitlink" could not be found by search; its settings advice (present pacing at capture rate, a fixed signal rate, VSync off, low latency on) is sensible in principle and unverified.

## What it means for the addons

1. **Our motion-driven features see zero motion, then full motion, on a duplicated source.** Sharpen less in fast motion, the Neural Rendering motion-compensated smoothing, the steady
   passes' motion trust band and the upscalers' history all use the motion between consecutive input frames. With A A B B the frame pairs alternate between no change and a whole step, so
   they could pulse at half the capture rate. **Not tested.** The test: the host test (`nr_hosttest`) or `nr_sreval` fed a clip with every frame doubled, against the same clip at its real
   rate; compare the output's frame-to-frame sharpness and flicker.
2. **A source-cadence diagnostic.** Count the input frames that are repeats (the downscaled difference under a threshold, as `tools/video_pacing.py` does for recordings) and log the true
   rate: "the source repeats every other picture: it is 30 fps in a 60 Hz feed". The panel could point at the capture setting. It cannot fix the cadence: dropping duplicates is for Lossless
   Scaling or the capture app. An addon can only skip one of Lossless Scaling's passes, which does not reshape its output timing, and we do not claim otherwise.
3. **`tools/video_pacing.py` can verify the table above** on a capture of Lossless Scaling's output from a doubled source (the stays should read 3, 1, 3, 1 at 120 Hz for the 60 capture case,
   not an even 2).
4. **A dedupe-aware mode is not ours to build now.** If it is ever wanted, it lives in the pre-dispatch path: recognise the repeat, hold the generated frame to the repeat's input, and let
   the real change step through at the true rate. Needs a look at what Lossless Scaling's dispatch order allows first.

## Result of test 1 (2026-10-08): no pulsing in the three upscalers

`nr_sreval ... dup=2` gives each recorded frame twice in a row (a 30 fps game in a 60 Hz feed) and scores the output's change from the picture before, on the repeats and on the new frames.
One recording (WoW, 2562x1442, 8 s, shrunk 1.5x and upscaled back, the shipped defaults: Sharpness at rest 0.5, Steady sharpening 0.6, Sharpen less in fast motion 0.5), 60 real frames:

| Upscaler | change on the repeats (levels of 255) | change on the new frames | detail, repeats vs new frames |
|---|---|---|---|
| FSR | 0.029 | 0.894 (the game's own 0.983) | 0.1 % apart |
| DLSS | 0.054 | 0.881 | 0.2 % apart |
| XeSS | 0.054 | 0.905 | 0.1 % apart |

A repeat of the input gives an output that is almost the same picture (the plain stretch: 0.000), and the sharpness does not alternate between repeat and new frames, so the motion-driven
passes do not toggle on a doubled source. Not covered: Neural Rendering (needs the model; `nr_nreval`), the live path (this tool measures motion from the frames; live, Lossless Scaling's flow
feeds the passes), HDR (`hdr=1`), a clip with fast camera motion, and 120 Hz (dup=4).

## Status

Test 1 done for the upscalers; the option `dup=N` is in `nr_sreval`. Open: the same for Neural Rendering and a live check; the cadence diagnostic (2); the output cadence (3).
