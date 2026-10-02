# Video Super Resolution (prototype)

NVIDIA's RTX Video Super Resolution (VSR) in place of Lossless Scaling's NIS scaling. VSR is a trained network that looks at one frame and sharpens the picture it upscales: text, interface and
still scenes come out crisp where the temporal upscalers (DLSS, FSR, XeSS in this project) can only stay smooth, because Lossless Scaling gives them no camera jitter to gather detail from
(issue #8). It is a prototype: off until you switch it on, built only with your own copy of NVIDIA's SDK, not part of the release packages.

## What you need

- An NVIDIA RTX card and a current driver.
- **Your own NVIDIA Video Effects SDK, x64** (NVIDIA's NGC catalog, the "VFX SDK Core" for Windows, the x64 build; the ARM64 build does not load), with the **Video Super Resolution** feature installed
  (`features\install_feature.ps1 -features nvvfxvideosuperres`, which needs an NGC API key). The SDK and its model are NVIDIA's, under NVIDIA's licences; nothing of them is in this repository or in the
  addon, and they are never fetched or shipped by us.
- Lossless Scaling with **NIS** as the Scaling Type, and the game in a window smaller than the screen (the same as the other upscalers).

## Setting it up

1. Build with the SDK in `addons/DLSS5NR01/external/vfx` (or `cmake -DVFX_DIR=<folder>`): the target `VSRUPSC` appears. `tools\deploy.ps1 -What vsr` puts `VSRUPSC.dll` in the Lossless Scaling folder (the game and
   Lossless Scaling closed). It is not part of `-What all`.
2. In LS Addon Manager, switch the addon on, open its settings, tick **Use Video Super Resolution**, and put the SDK's folder in the box (the folder that holds `bin` and `features`). Restart Lossless Scaling after
   changing the folder.
3. Start a game with NIS. The panel's status line says what it does, for example `VSR quality 1, 2560x1440 to 3840x2160: 3.1 ms a frame on the render thread`.

## What it does and costs (measured on an RTX 4070 Ti SUPER)

| Mode | 2560x1440 to 3840x2160 | Detail kept | Frame to frame |
|---|---|---|---|
| Bicubic (VSR's own, no AI) | 0.9 ms | 65 % | the reference |
| **Low** | 2.3 ms | 82 % | +25 % shimmer |
| Medium | 2.9 ms | 82 % | +50 % |
| High | 6.4 ms | 80 % | +50 % |
| Ultra | 8.4 ms | 82 % | +50 % |

Low is the mode: Medium to Ultra shimmer more and keep no more detail. The hand-over of a Direct3D 11 frame to VSR's CUDA buffers and back adds about 0.65 ms. The addon runs VSR **synchronously on Lossless
Scaling's render thread**, on every frame the NIS pass is dispatched for (real and generated), so with frame generation x3 that is three runs per real frame. See `handoff/BACKLOG.md` for the plan (real frames only,
a motion gate, a device of its own) and what is not measured.

## Safety

- Any failure hands the pass back to NIS and says why in the panel and the log.
- `running.txt` in the addon folder marks a session in which VSR ran; if Lossless Scaling went down with it there, VSR stays off at the next start until you delete the file (the panel's *Try again* does).
- The first run in a session builds the effect for the frame size (about 0.8 s, one stall).
- HDR frames go through their SDR view (8-bit), so highlights above the SDR white are rolled off.
