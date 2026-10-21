# Video Super Resolution

NVIDIA's RTX Video Super Resolution (VSR) in place of Lossless Scaling's NIS scaling. VSR is a trained network that looks at one picture and sharpens what it upscales: text, interface, images and film come out
crisp where the temporal upscalers (DLSS, FSR and XeSS in this project) can only stay smooth, because Lossless Scaling gives them no camera jitter to gather detail from (issue #8).

**It is made for pictures that are not a game:** movies and video, images and photographs, slides, text, an interface, a still scene. A game that moves a lot is better served by the
temporal upscalers; for a game this addon has a *motion gate* (below) that keeps Lossless Scaling's own NIS where the picture moves.

## What you need

- An NVIDIA RTX card and a current driver.
- Nothing else to download: the addon's `vfx` folder holds the files of NVIDIA's Video Effects SDK it uses (x64, with the Video Super Resolution feature), with NVIDIA's licences (`vfx\license`, and NOTICE.md).
  Your own copy of the SDK works too: put its folder (the one that holds `bin` and `features`) in the addon's settings and restart Lossless Scaling.
- Lossless Scaling with **NIS** as the Scaling Type, and the picture in a window **smaller than the screen** (a player or an image viewer at 1920x1080 on a 4K screen, for example). At the screen's own size
  there is nothing to enlarge and the addon says so.

## Using it

1. In the manager's Addons tab switch **Video Super Resolution** on. (Only one upscaler runs at a time: switching it on switches DLSS, FSR and XeSS off.)
2. Choose **NIS** as the Scaling Type in Lossless Scaling and scale the window of your player or viewer.
3. The panel's status line says what it does, for example
   `VSR quality 1 everywhere, 1920x1080 to 3840x2160: 0.6 ms a frame on the render thread (480 frames; 120 pictures worked on, 360 repeats put out again)`.
4. Compare with Lossless Scaling's own NIS: **Ctrl+Shift+F9** steps through NIS, this addon (gated and everywhere) and every other upscaler that is loaded, with a label in the corner.

## What it costs (measured on an RTX 4070 Ti SUPER)

| Mode | 2560x1440 to 3840x2160 | Detail kept | Frame to frame |
|---|---|---|---|
| Bicubic (VSR's own, no AI) | 0.9 ms | 65 % | the reference |
| **Low** | 2.3 ms | 82 % | +25 % shimmer |
| Medium | 2.9 ms | 82 % | +50 % |
| High | 6.4 ms | 80 % | +50 % |
| Ultra | 8.4 ms | 82 % | +50 % |

**Low** is the default: Medium to Ultra shimmer more and keep no more detail. The hand-over of a Direct3D 11 frame to VSR's CUDA buffers and back adds about 0.65 ms. The addon runs VSR
**synchronously on Lossless Scaling's render thread**, on every frame the NIS pass is dispatched for, so the cost matters for a game and hardly at all for a film.

## A picture that has not changed is not worked on again

Lossless Scaling presents a frame every refresh of the screen (60 times a second and more), and VSR would work on all of them. But a still image is the same picture every time; a paused video the same; and a
film at 24 or 30 pictures a second shown at 60 Hz or more gives each picture two to six times in a row. So after the picture is made into the 8-bit view VSR takes, the addon **compares it on the card with the picture
VSR last worked on**: if no more than about a twenty-thousandth of the pixels differ, and those by no more than one level (video noise, a cursor), VSR does not run and its last result is put out again.

- A still image or a paused video costs a compare and a copy (about 0.4 to 0.5 ms at 1440p to 4K) instead of VSR's 2 to 3 ms, and VSR's CUDA work stops altogether.
- A film at 24 fps on a 60 Hz screen runs VSR for 24 of the 60 frames a second; at 30 fps on 120 Hz, 30 of 120.
- It is **exact where it matters**: a picture that has changed (a new film frame, a scroll, a cut) is never taken for a repeat, and a slow fade is caught because the compare is against the last picture VSR worked on, not
  the frame before.
- Setting `skipRepeats` (the panel's *Do not work again on a picture that is the same as the last*) turns it off; `repeatTolerance` (0 to 8 levels, default 1) is how much a pixel may differ by.

## The motion gate (for games; off by default)

The first live test (2026-10-02, WoW on an RTX 4070 Ti SUPER) found what the tests suggested: VSR sharpens a still picture, and on what moves it blurs and shifts colour, so in a game it was about on a level with FSR or XeSS
in motion. With **Only where the picture is still (for games)** (the setting `motionGate`), Lossless Scaling's NIS runs as usual and VSR's picture is blended over it only where the frame did not change from the pass before
(a 3x3 neighbourhood of the input pixel, so the edge of something that moves stays NIS). The interface and a standing character keep VSR while the world moves around them; when the camera stops, VSR takes over the
whole picture again. **Motion sensitivity** (`gateHigh`, 0.05; `gateLow` is a quarter of it) is how much a pixel has to change to count as moving. The price is NIS's dispatch, a copy of its picture and the blend (a few tenths of a
millisecond) on top of VSR's run.

With the gate on, VSR also runs **at most every 20 ms** (`vsrIntervalMs`, 0 = every pass): where the picture is still its last result is still right, so with frame generation it does not run for each of the several passes of a real
frame. The gate keeps a **motion memory** per input pixel (the change from the pass before added to 85 % of what was remembered), so a slow movement, which changes a pass by less than any threshold, adds up to the movement it is.
Test host: `vsr_fade` and `vsr_fade_nogate`.

## Safety and limits

- Any failure hands the pass back to NIS and says why in the panel and the log.
- `running.txt` in the addon folder marks a session in which VSR ran; if Lossless Scaling went down with it there, VSR stays off at the next start until you delete the file (the panel's *Try again* does).
- The first run in a session builds the effect for the frame size (about 0.8 s, one stall).
- VSR takes an 8-bit picture. HDR frames go through their SDR view, so highlights above the SDR white are rolled off: for an HDR film the temporal upscalers, which work in light, keep more of the highlights.
- It enlarges: at the screen's own size (or larger than the screen) nothing is done.
- Test host scenarios: `vsr`, `vsr_repeat`, `vsr_norepeat`, `vsr_nogate`, `vsr_4_3`, `vsr_move`, `vsr_fade`, `vsr_fade_nogate`, and the comparison scenarios (`tools/run_hosttest_matrix.py`).
