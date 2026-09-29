# The DLSS, FSR and XeSS Upscalers

Three addons, built from Neural Rendering's sources, that take the place of Lossless Scaling's **NIS** scaler with a temporal upscaler:

| Addon | Upscaler | Needs | Comes with |
|-------|----------|-------|------------|
| **DLSS Upscaler** (`DLSS4DLAA`) | NVIDIA DLSS Super Resolution (models K, M and E) | an NVIDIA RTX card | NVIDIA's DLSS runtime 310.9.1 (`dlss\nvngx_dlss.dll`, NVIDIA's licence) |
| **XeSS Upscaler** (`XESSUPSC`) | Intel XeSS Super Resolution | any DirectX 12 card with Shader Model 6.4 (Intel Arc, NVIDIA, AMD) | Intel's XeSS runtime 2.0.2 (`xess\libxess.dll`, from the XeSS SDK 3.0.2, Intel Simplified Software License, signed by Intel) |
| **FSR Upscaler** (`FSR3UPSC`) | AMD FidelityFX Super Resolution 3.1, or FSR 4 | any DirectX 12 card (AMD, NVIDIA, Intel) | AMD's FidelityFX runtime (`fsr\amd_fidelityfx_dx12.dll`, FSR 3.1.4, FidelityFX SDK v1.1.4, MIT, signed by AMD), and FSR 4.1.1b, the OptiScaler team's build (`runtimes\FSR\0dd77d9c`, AMD's SDK licence, not signed) |

All three come in the download switched off: switch on the one you want. DLSS and FSR were developed and tested with World of Warcraft:
Forever, Fallout: New Vegas (Tale of Two Wastelands) and Silent Hill f (HDR), on an RTX 4070 Ti SUPER. XeSS (new in 0.9.12) has passed the
offline tests but has not been tried in a game yet.

A temporal upscaler builds each picture from several frames, so it has to know where every pixel was in the frame before. A game with DLSS or
FSR built in tells it; Lossless Scaling does not. These addons **measure that motion from the frames themselves**, so they work in any game
Lossless Scaling can scale, with frame generation on or off, and need nothing from the game.

## Setting up

1. In the manager's addon list, switch on **one** of the three. They take the same pass, so turning one on turns the others off. Any of them works
   beside DLSS 5 Neural Rendering.
2. In Lossless Scaling's profile for the game, choose **NIS** as the Scaling Type. The addon replaces exactly that pass; with another scaler
   it waits and does nothing.
3. Run the game **in a window smaller than the screen**, so there is something to upscale. 2560x1440 on a 4K screen is 1.5x, the upscalers'
   quality mode and the best starting point; 1920x1080 on 4K (2x) works too, softer. A borderless window the size of the screen (for example
   World of Warcraft's *Windowed (Fullscreen)* at 3840x2160 on a 4K screen) gives 1:1: nothing is upscaled, and the addon runs as
   anti-aliasing (DLSS's DLAA, FSR's native AA, XeSS's anti-aliasing setting). Avoid exclusive fullscreen: Lossless Scaling cannot scale over it.
4. Frame generation can be on or off.
5. Scale the game as usual. The addon's panel (Upscaling) shows `DLSS upscales 2560x1440 -> 3840x2160 (x1.50) ...` (or `FSR 3.1.4 upscales ...`, `FSR 4.1.1b upscales ...`, `XeSS ...`) once it runs.

In-game settings that help: the game's own **multisampling** (MSAA) if it has one. It draws what no upscaler can put back, such as wires and
fences thinner than a pixel; in Fallout: New Vegas 8x (`iMultiSample=8`) made a large difference, and a game held back by its processor
has the graphics card to spare for it. A post-process anti-aliasing (FXAA, CMAA) adds little: the upscalers rebuild edges their own way
(the addon's **Edge smoothing**, after the upscaler, is there for games with no anti-aliasing at all). Keep the game's own render scale at
100% so it does not upscale first.

## Settings (the addon's panel)

- **FSR version** (FSR Upscaler only, at the top of the panel): *FSR 3.1.4 (AMD, shipped)* or *FSR 4.1.1b INT8 with the RDNA 2 fix*. FSR 4 is
  AMD's machine-learning upscaler; this build of the OptiScaler team's runs it on cards AMD's own FSR 4 does not (NVIDIA's too). It follows
  a moving picture better (test host: a sliding picture 11.1 levels off the truth against FSR 3.1's 14.2) and costs more. Switching takes
  about a second while the game runs; the panel names the FSR that runs. It is the same choice as the **+** next to FSR in the manager's
  Runtimes list, where other files can be added too.
- **DLSS model** (DLSS Upscaler only): **Auto** (the default: E at 1:1, L when upscaling), or *K* (NVIDIA's default), *L*, *J* (DLSS 4), *M* (DLSS 4.5)
  or *E* (DLSS 3's CNN model). Lossless Scaling's frames carry no camera jitter, which the DLSS 4 models expect, so they behave differently here
  than in a game. Measured on recordings (`nr_sreval`): **L** is the steadiest when upscaling (the only one that does not drift from the picture in
  a slow pan) but **pulses on a four-frame cycle at 1:1**, which shows as flicker while you move (seen in World of Warcraft, 2026-09-28); **E**
  does not, is the lightest, and keeps a still picture crisp, so Auto takes E at 1:1 (DLAA) and L when upscaling. K and J trail a little more; M
  is heavier (about twice K's cost) and did worst here. Compare them in your game. *DLSS version*, at the top of the panel,
  picks the runtime (the shipped one, or one added with **+** in the Runtimes list).
- **Steady in fast motion** (on) and **Lean from**: without the game's camera jitter an upscaler's memory of earlier frames adds no detail in
  motion, only trailing (soft doubled outlines on leaves and edges). Where the picture moves more than a small amount, the upscaled picture
  leans on the new frame: from about 1 px a frame when upscaling (0.05 % of the frame's width) and from about 4 px at 4K at 1:1 (0.1 %). *Lean from*
  sets it by eye (0: automatic). On a recorded fast turn, upscaled 1.5x, this took DLSS from 43.2 to 47.2 dB (a plain stretch: 43.9),
  measured where trailing shows.
- **Sharpness at rest** (off): when the picture stands still the upscaler's own picture shows, and it is softer than a plain stretch of the game's frame: Lossless Scaling gives it no camera jitter, so it has no extra detail to find, only its smoothing (anti-aliasing) to give. Held still on recordings (`nr_sreval still=1`), DLSS scores 32.5 dB against 33.6 for a plain resample, with 73 % of the game's detail against 80 %; this slider puts that share of the plain stretch into the picture even at rest (0.25: 32.9 dB, 0.5: 33.2, 1: 33.6). It trades the upscaler's anti-aliasing at rest for sharpness; sharpening (above) adds on top. Try 0.3 to 0.5 if a still scene looks soft (issue #8).
- **Crisp edges when moving (test)** (off): the picture the upscaler leans on where the camera moves is the frame stretched to the screen; by default with a smooth filter (Catmull-Rom), with this on with AMD's FSR 1 edge-adaptive filter (EASU), which keeps diagonal and thin edges cleaner and crisper. On recordings the two score the same on shimmer and detail (`nr_sreval lean=easu`), and on the host test's sliding picture EASU is closer for FSR (11.7 against 14.2 levels off) and equal for DLSS; the difference is in how edges look, so flip it while you turn the camera.
- **Motion by shape (test)** (off): the motion is found by matching each small block of the frame before to this one; this matches their
  shape and edges rather than their brightness, for scenes where the light changes (spell effects, flashes, fades). Neutral on the recordings
  tried. Switching it on makes a second version of the search shader, on a thread of its own (it takes several seconds); the plain one runs meanwhile.
- **Sharpening** (0.3 to start with): DLSS 4 has no sharpening of its own, so the DLSS addon sharpens its picture with the contrast-adaptive
  (CAS) formula; the FSR addon uses AMD's own RCAS. NIS sharpens too (Lossless Scaling's Sharpness), so without it the upscaler can look
  softer next to NIS. 0.2 to 0.6 is the useful range. Ctrl+Shift+F2 / F3 lower and raise it in the game.
- **Motion**: *Measured from the frames* (the default), *Lossless Scaling's frame generation* (its flow: coarser, a quarter of the game's size,
  and only with frame generation on), or *None* (the upscaler assumes nothing moves: sharp when still, smeared when the camera turns; there to
  compare).
- **Stability** (off to start with): less shimmer on thin lines, wires and leaves, for a little more trailing behind what moves. The motion
  measurement then judges trust by a pixel's surroundings rather than the pixel itself, so flicker is no longer reported as "do not trust the
  history here" and the upscaler averages it out; FSR 3.1 also keeps more history and reacts less to small shading changes (AMD's tuning keys).
  FSR 4 keeps its history its own way and takes none of this: the slider is greyed out while FSR 4 runs.
  Real motion is followed as before. 0.5 is a good start. A thin line that moves (a wire swaying in the wind, a branch) is kept out of
  it: its history, the line at other sub-pixel places, would only blur it, so where a one-pixel ridge sits on moving content the upscaler
  still leans on the current frame (test host: a swaying line 8.8 levels off the true picture with stability 0.5, as without it; 10.2
  before this).
- **Edge smoothing** (off to start with): anti-aliasing along the edges of the upscaled picture, for games without anti-aliasing of
  their own. It finds where the brightness steps, which way the edge runs and how far, and blends across it by the part of a pixel the true
  edge would cover (our own pass, in the family of FXAA). It runs after the upscaler, before the sharpening: smoothing the game's frame
  first did nothing, as both upscalers rebuild edges their own way. It helps FSR 3.1 most (a hard slanted edge 27% closer to the ideal at
  1.5x, 35% at 1:1); DLSS already smooths edges itself. A fraction of a millisecond. The game's own MSAA is better where there is one.
- **Vibrance, saturation, shadows, highlights, brightness, contrast, gamma**: the picture's colour and tone, as Neural Rendering's Picture
  controls do them, applied to the frame on its way to the upscaler (they ride on a pass that already runs, so they cost nothing). While
  DLSS 5 Neural Rendering is on, its own Picture section is in charge and these are greyed out, so nothing is applied twice.
- **Keep these settings per game** (on): sharpening, stability, edge smoothing, colour and tone, the DLSS model and the motion are kept for each game
  (by its program name) and come back when it takes focus. Only the window being scaled counts (its inside is the frame's size), so a chat
  program or browser in front is not taken for a game. A game seen for the first time keeps the settings in use; a change made in the
  panel counts for the game played last. The list under it shows each game's settings, with a button to forget one.
- **Frame encoding** (Automatic): for HDR games. 16-bit float frames are scRGB, 10-bit ones HDR10 when Windows runs the display in HDR,
  8-bit ones SDR. An HDR frame is upscaled as light in the upscaler's own HDR mode (since 0.9.10; before, in an SDR view, which dimmed
  highlights) and the picture goes back in the frame's own encoding, so highlights keep their brightness. Set SDR or HDR only if the picture comes out washed out, too dark or too
  bright; Technical status shows the frame's format and what was decided.
- There is no Enable box in the panel: the upscaler is switched on and off in the manager's addon list, like every addon.
- **Before / after** (Ctrl+Shift+F7): switches between the upscaler and Lossless Scaling's own NIS while you play. The hotkeys work only while
  the upscaler is actually upscaling.

## What it costs

Measured on an RTX 4070 Ti SUPER in World of Warcraft: Forever (GPU time a presented frame, everything the addon does):

| | DLSS (model K) | FSR 3.1 | XeSS |
|---|---|---|---|
| 2560x1440 -> 3840x2160 | about 2.4 ms (before the motion estimate got cheaper) | about 1.85 ms | not measured in a game yet |
| 3840x2160, 1:1 (anti-aliasing) | about 3.2 ms (the same) | about 2.2 ms | not measured in a game yet |

Of that, the motion estimate is about 0.35 ms at 2560x1440 and 0.7 ms at 3840x2160. The panel and the log show the addon's own numbers.
The addon adds one frame of latency: the picture shown is the one the upscaler finished for the frame before, so Lossless Scaling never waits.

## What to expect

- **In motion** the measured motion makes the difference: without it, anything moving smears. With it, the picture holds together while the
  camera turns, and where the motion cannot be trusted (background just uncovered, particles, transparent effects) the upscaler is told to
  lean on the current frame instead of its history.
- **Fine detail**: a game with DLSS built in shifts its camera by a fraction of a pixel every frame, which lets DLSS rebuild detail finer
  than the render size. A captured frame has no such shifts (and no depth), so the upscalers work from what each frame shows; camera motion
  gives them some of the same variety. The same goes for anti-aliasing: a line thinner than a pixel is simply missing in parts of the frame,
  and no upscaler can draw it back. Turn on the game's own anti-aliasing (MSAA) if it has one: in Fallout: New Vegas 8x (`iMultiSample=8`)
  made a large difference, and the upscaler on top of it looks better again. Stability then calms what shimmer is left.
- **Text and HUD** are part of the captured frame, so the upscaler treats them like the rest of the picture; thin text can come out a little
  softer or dimmer than with NIS (FSR 3.1 keeps it crisper than DLSS). A game with DLSS built in draws its HUD after upscaling. Keeping NIS for
  HUD areas is on the roadmap.
- On a clean, low-detail game (World of Warcraft) the difference to NIS is small. Games with foliage, fine detail and busy motion show it more.
- **A window of another shape than the screen** (a 4:3 game on a 16:9 screen): Lossless Scaling scales it into part of the screen. The
  upscalers read that part from NIS's own settings (its viewports) and upscale into it, leaving Lossless Scaling's borders as they are. The
  log says so once ("the window is scaled into part of the screen"), or why NIS was kept.

## When something is wrong

The addon writes `logs\DLSS4DLAA.log`, `logs\FSR3UPSC.log` or `logs\XESSUPSC.log` in the Lossless Scaling folder.

| What you see | Why, and what to do |
|--------------|---------------------|
| The panel says it is waiting for the NIS pass | Lossless Scaling's Scaling Type is not NIS, or scaling has not started. |
| No difference at all | At 1:1 (the game fills the screen) there is nothing to upscale; run the game in a smaller window. Check the panel says it upscales, and try the Before / after hotkey. |
| The panel keeps waiting although NIS is chosen | Look for "NIS pass on part of its output" in the log: with a window of another shape than the screen the upscaler reads NIS's viewports first (a frame or two), and says there if they did not add up, in which case NIS stays. A window of the screen's shape (2560x1440 on a 3840x2160 screen) always works. |
| The panel names a card that is not NVIDIA's (DLSS Upscaler) | Lossless Scaling runs on that card. Set its Preferred GPU to your NVIDIA RTX card, or use the FSR or XeSS Upscaler, which run on any card. |
| An HDR game looks washed out, too dark or too bright | Set *Frame encoding* (under Upscaling) to HDR or SDR by hand, and tell us which it needed, with the log. |
| A black picture | Should not happen since 0.9.1. The log's `probe:` lines say how bright the frame the upscaler got and the picture it made are: 0 of 255 means black. Please report it with the log. |
| Smear when moving | Check Motion is *Measured from the frames*. The log's `motion estimator:` lines give the average motion found and how much of the picture was marked untrusted. |
| FSR 4 looks wrong, costs too much or does not start | Set *FSR version* back to FSR 3.1.4. A chosen runtime that has gone missing falls back to the shipped one by itself (the log says so). |
| You want us to see it | Turn on *Recording* (the addon's panel), make it happen, press Ctrl+Shift+F1 and send the `.lsrec` file from `Videos\Lossless Scaling`: it holds the frames as they went to the upscaler, and we can play them back. |
| "... could not run: ..." in the panel | The runtime is missing from the addon's `dlss`, `fsr` or `xess` folder (reinstall the addon), or, for DLSS, the card is not an NVIDIA RTX card. NIS runs as usual meanwhile. |

## How it works

On Lossless Scaling's side (`src/addon/scaler11.cpp`), at the NIS pass (recognised by what is bound: the frame, NIS's two 2x64 coefficient
tables, the picture at the screen's size, one thread group per 32x24 pixels), the addon reads the frame into a shared texture with a small
compute pass through the NIS pass's own view of it, signals a shared fence, copies the newest finished picture into the pass's output and
skips NIS. Everything else runs on a Direct3D 12 device of the addon's own (`src/engine/sr_engine.cpp`), which waits for that fence on the GPU:

1. **The motion estimate** (`src/engine/flow_estimator.cpp`): the frame's brightness at its own size and halved down to about 64 pixels
   wide; from the smallest size up, every 4x4 block finds where its 8x8 surroundings were in the frame before (seeded from the size below,
   with a small cost for straying from that guess); a fraction of a pixel at half size; a 3x3 vector median; and, at the game's size, every
   pixel picks the best of "not moving", its own block's vector and the three nearest blocks' by how well its 3x3 surroundings match. The
   same step gives the **distrust mask**: where even the best vector leaves a pixel unlike the frame before.
2. **The upscaler**: DLSS (NVIDIA's NGX API), FSR 3.1 / 4 (AMD's FidelityFX API) or XeSS (Intel's XeSS API, at the finest quality setting that takes the game's size), given the frame, the motion vectors, a flat depth and the
   distrust mask (DLSS's bias-toward-current-colour mask, FSR's reactive mask), with no camera jitter.
3. The DLSS addon's sharpening pass (FSR sharpens inside its own pass).

Why it is built this way, briefly: NVIDIA's D3D11 DLSS run on Lossless Scaling's own device crashed it, so the upscaler has a device of its
own; making Lossless Scaling's queue wait for the upscaler is avoided (one frame of latency instead); and with frame generation off the NIS
pass reads Lossless Scaling's capture directly, a keyed-mutex texture shared from its capture device, which a plain copy read as black, so the
frame is read the way NIS reads it. `scalerHandoff` in the addon's config (0 the default; 1 GPU wait; 2 upscaler runs but NIS stays; 3 NIS
runs and the picture is pasted at Present) keeps the variants that found this. [architecture.md](architecture.md) has the details.
