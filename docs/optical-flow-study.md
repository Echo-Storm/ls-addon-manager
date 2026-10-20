# NVIDIA's optical flow hardware against our own flow estimator (2026-10-20)

Follow-up to docs/mfg-super-motion-study.md: MFG Super Motion offers NVIDIA's optical flow hardware (NVOFA) as one of its motion sources. Is it a better source than the flow estimator under EchoFG and the upscalers (`src/engine/flow_estimator.cpp`)? Lab tool: `tools/flowlab` (not part of any build; needs the four public headers in `addons/DLSS5NR01/external/nvof`, see its README, and the driver's `nvofapi64.dll`). Scorer: `tools/flowlab/score_flow.py`.

## What it measures

On the clips of 2026-10-19 (WoW, 2560x1440, Compact, the middle 1280 x 720 cropped): the even frames are the real frames, the odd frames are the truth between them (a 30 fps game's pair, 33 ms apart). For each pair (A before, C after) each source makes the flow at C pointing back to A. Both are scored with the same code:

- **photometric error**: mean |C(x) - A(x + F(x))| (levels of 255): does the vector find where the pixel came from;
- **interpolation**: the frame between made as `0.5 * (A(x + F/2) + C(x - F/2))` (identical for every source), PSNR and brightness correlation against the true frame between. With no motion at all this is the plain blend.

## Results (29 pairs a clip; PSNR dB / correlation)

| clip | blend (no motion) | **our estimator** | NVOFA, brightness plane at 1/2 size | NVOFA at full size |
|---|---|---|---|---|
| 23-16-58, camera moving | 23.72 / 0.711 | **25.71 / 0.812** | 25.27 / 0.784 | 23.76 / 0.693 |
| 23-18-17, sunset, high contrast | 27.88 / 0.597 | **30.26 / 0.791** | 28.93 / 0.696 | 28.65 / 0.692 |
| 23-16-37, building, small steps | 27.42 / 0.854 | **29.69 / 0.900** | 28.68 / 0.874 | 27.00 / 0.830 |
| 23-17-18, canyon wall, nearly still | 30.18 / 0.964 | **34.12 / 0.985** | (not run) | 32.81 / 0.968 |

- **Ours beats the hardware on every clip**, by 0.4 to 1.3 dB at the best NVOFA setting, and its photometric error is 4-5 times lower (2.1 against 11 on the camera clip).
- **Fusing the two** (per pixel, the vector whose 7 x 7 surroundings match better) gives exactly our numbers: the hardware's vectors add nothing our estimator does not already have.
- **Settings did not matter much**: scale 2 and 4 are the same (25.27 / 25.30 dB on the camera clip), slow / medium / fast perf 25.27 / 25.31 / 24.99, output grid 1 / 2 / 4 all 25.27. Full size is worse on fast motion: the hardware's search is of limited reach (the camera clip moves 30-100 pixels between real frames), so it has to be given a smaller picture, which is what MFG Super Motion does too (its `LF_OFA_SCALE`, "flow luma divided by N").
- **Cost**: 0.4 ms for a 1280 x 720 pair (execute and readback, D3D11 interop), on its own engine, leaving the shaders free. That is the one thing in its favour: our estimator costs GPU shader time (about 1-2 ms at this size on this card). It could be a cheap *seed* for our search, not a better answer.

## How to drive it (things that cost time, kept so they are not lost)

- Direct3D 11 interop is the easy path (`NvOFAPICreateInstanceD3D11`, `nvOFRegisterResourceD3D11`, `nvOFExecute`; no fences). The output texture is the format the API reports (`R16G16_SINT`), vectors in S10.5 fixed point (divide by 32), one per `grid` pixels.
- **The input must be a brightness plane** (`NV_OF_BUFFER_FORMAT_GRAYSCALE8`, an `R8_UNORM` texture). With `ABGR8` (an `R8G8B8A8` texture, the memory order matching) the field came out as nonsense (a constant 24 px shift with a shear); with a gray plane it is right. Not understood why; the cheap fix is to make the plane yourself.
- The sign: with input = the later frame and reference = the earlier, forward flow came out as `x_in_A - x_in_C` after the scorer picked the sign by photometric error (+1 at scales 2 and 4); at full size it picked -1 because the field was mostly wrong there. Check it rather than assume it.
- `disableTemporalHints = 1` here (each pair stands alone); a real stream would leave the hints on.
- `nvOFGetCaps(NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES)` returned an empty mask on this driver (617.42), but grids 1, 2 and 4 all work.

## What this means

NVIDIA's hardware flow is not a way to a better picture on this footage; a hybrid does not help either. Where it could matter is **cost**: a smaller GPU share for the motion search (seeding the coarse levels of our pyramid, or replacing them at low settings) on a card that is already full, which is the GPU-limit use case (the manager's feature of 2026-10-19). That is a speed idea, not a quality one, and it would add a dependency on a driver component that exists on NVIDIA cards only. Not pursued for 1.0. Board card E14.

Other things this study does not cover: the 4K case, the flow at the *middle* frame (needs the later frame and the middle's truth), HDR frames, AMD's and Intel's equivalents (AMD's driver-level optical flow, Intel's XeSS-FG library has its own).
