# DLSS 5 Neural Rendering (DLSS5NR01)

> [!IMPORTANT]
> **You provide the model file yourself.** This addon needs your own copy of `nvngx_dlssnr.dll`. It is **not included**, it is **never downloaded**, and this project does not say
> where to get it. Put your copy in the Lossless Scaling folder, next to `LosslessScaling.exe` (the addon's **Browse for the model file...** button copies it there for you),
> then press **Test compatibility** in the addon's panel.

> [!NOTE]
> **Tested with World of Warcraft: Forever and Fallout: New Vegas.** This project was developed and tested against **World of Warcraft: Forever** (the beta; it runs as `WowB.exe`) and **Fallout: New Vegas** (Tale of Two Wastelands), on Lossless Scaling 3.2.2.0, Windows 11 and an RTX 4070 Ti SUPER. Other games and setups are untested.

An addon for LS Addon Manager, the addon manager for Lossless Scaling, that runs NVIDIA DLSS 5
Neural Rendering (DLSSNR) on the frames Lossless Scaling captures and applies the result to every
frame Lossless Scaling presents, real and generated. Nothing is injected into the game. No ReShade.
Lossless Scaling never waits for the model.

Built for a dual-GPU setup (game on one card, Lossless Scaling and this addon on the other), and it
also works on a single GPU and on hybrid laptops. The model runs on whichever NVIDIA GPU Lossless
Scaling uses for LSFG.

- **Read-only tap.** The addon copies each new real frame out of LSFG's pipeline together with
  LSFG's own optical flow. Lossless Scaling's frame is never written.
- **Free-running model.** DLSSNR runs on its own D3D12 queue and produces a *delta* (model minus
  input). If a run is still on the GPU when the next frame arrives, that frame is skipped.
- **Present-time compose.** The newest finished delta is added to every frame Lossless Scaling
  presents, moved by LSFG's flow to where the content sits in that frame. A slow model costs
  freshness of the enhancement, never frame rate or latency.

Documentation: [User guide](docs/user-guide.md) · [Architecture](docs/architecture.md) ·
[Building](docs/building.md) · [What the model listens to](docs/dlssnr-knobs.md) ·
[Changelog](CHANGELOG.md)

## What you need

- Lossless Scaling 3.x with LS Addon Manager installed and working.
- An NVIDIA RTX GPU running LSFG. RTX 30 was the development hardware (Ampere runs the model in
  FP16, so it is the slow case). RTX 20 should behave the same. RTX 40/50 are expected to be
  considerably faster but were not tested by the author.
- The DLSSNR snippet, `nvngx_dlssnr.dll`. NVIDIA has not released it publicly. This project does
  not ship it, link to it, or help you find it. Put your copy in the Lossless Scaling folder (or
  point the addon at it in *Advanced*).

## Install

1. Download the latest release zip and run `LSAddonManagerSetup.exe` from it: it installs the manager and this addon, with backups. Or do it by hand: extract the zip and copy the
   `addons\DLSS5NR01` folder into your Lossless Scaling folder so you have:
   ```
   <Lossless Scaling>\addons\DLSS5NR01\DLSS5NR01.dll
   <Lossless Scaling>\addons\DLSS5NR01\nvngx.dll_dlss5nr01.dll
   <Lossless Scaling>\addons\DLSS5NR01\nr_selftest.exe
   <Lossless Scaling>\addons\DLSS5NR01\addon.json
   ```
2. Put your own copy of `nvngx_dlssnr.dll` in the Lossless Scaling folder (next to `LosslessScaling.exe`), or open the addon's panel and press
   *Browse for the model file...* to copy it there. Then press *Test compatibility*.
3. Start Lossless Scaling, open LS Addon Manager, enable *DLSS 5 Neural Rendering*
   and open its settings.
4. Start scaling a game. The status line goes from *waiting for LSFG dispatches* to *engine:
   loading model...* to *running*. The first model load takes a few seconds.

The addon logs to `<Lossless Scaling>\logs\DLSS5NR01.log`. The [user guide](docs/user-guide.md)
walks through every setting, how to pick the working scale, and what each status and log line means.

## Cost

On an RTX 3090 a model run costs about 10 ms plus 7 ms per megapixel of model input, whatever the
pixels are; on an RTX 4070 Ti SUPER about 2.7 ms plus 1.8 ms per megapixel. The *Working scale* is the only cost lever: the frame is shrunk by it before the model
sees it, and the delta is upsampled back at present time. At 5120x1440 a working scale of 0.35
gives a 14 to 17 ms run, which keeps up with a 48 fps game. At a working scale past the frame
interval the model skips frames and the previous delta is carried forward by the flow; the panel
shows how many frames the model keeps up with. *Auto* (off by default) keeps the model within a time budget by lowering the
working scale when it runs over and raising it back when there is room. Changing the working scale has the model made again on a
thread of its own, so the game never stalls for it.

Because the model shares the GPU with LSFG, a heavy model run can still delay LSFG's own work on
the same card. *LS's GPU work first* raises Lossless Scaling's GPU priority so LSFG's passes and
presents pre-empt the model.

## How it works

```
capture k ─┬─ LSFG pyramid pass on frame k            <- TAP: copy frame k and LSFG's flow to the model's
           │                                             input, start a model run (skipped if the previous
           │                                             run is still on the GPU). LS's frame is never written.
           ├─ LSFG flow and interpolation passes
           └─ Present: generated a, generated b, real k  <- PRESENT HOOK: newest finished delta + this frame,
                                                           warped by the flow to where the content sits
```

An inline hook on d3d11's `Dispatch` recognises LSFG's per-real-frame pass by shape. NT-shared
textures and three shared fences connect Lossless Scaling's D3D11 device with the model's D3D12
queue, with GPU-side waits only. A patch on the swap chain's `Present` vtable slots sees every
presented frame; a compute pass on Lossless Scaling's own device adds the delta. The full design,
including why each hook is the kind it is, is in [docs/architecture.md](docs/architecture.md).

## Recording (for bug reports)

*Recording* in the panel keeps the last few seconds of the frames the addon receives (the game's frames, before anything is done to
them) in memory, losslessly compressed, and saves them as a `.lsrec` file on its button or Ctrl+Shift+F1. It's off by default. On, it
uses some processor time and up to the memory set there (about 2 to 3 GB for 5 seconds of 1080p at 120 frames a second). Files go to
`Videos\Lossless Scaling`. Sending one with a bug report lets the problem be replayed on another computer:

```
nr_lsrec info <file.lsrec>
nr_lsrec export <file.lsrec> <folder> every=10
nr_hosttest DLSS5NR01.dll - <model path> offframes=150 replay=<file.lsrec> replayOut=<folder>
```

## Limitations

- HDR frames (scRGB 16-bit float, HDR10 10-bit) are worked on through an SDR view of them relative to Windows' SDR content
  brightness; only the change goes back, so highlights keep their brightness. If an HDR picture comes out washed out or too dark,
  set *Frame encoding* (Advanced) by hand. Not yet tried on a real HDR display. The upscalers take 8-bit frames only.
- The 310.8 DLSSNR build ignores depth and does no upsampling of its own; the addon feeds LSFG's
  optical flow as motion vectors and does the scaling itself.
- Ampere runs the model in FP16; expect the working scale to sit between 0.3 and 0.5 there.
- Only one build of the DLSSNR snippet creates its feature on Ampere. If the engine reports
  `FeatureNotSupported` at `CreateFeature`, you have the other one.

## Roadmap

A short list of what is planned. Ideas are listed here so they are
not lost; none of them is promised.

- **Auto model resolution: done in 0.8.0** (*Auto: keep the model within a time budget*). Room to grow: a budget per game,
  and "keep a share of the GPU free" as well as a model time.
- **The DLSS and FSR Upscalers** are built from these sources as two more addons: NVIDIA DLSS or AMD
  FSR 3.1 in place of Lossless Scaling's NIS scaler, with motion measured from the frames. See [docs/upscalers.md](docs/upscalers.md).
  (They began as DLSS 4 DLAA on the captured frame, which changed nothing visible; [research](../../docs/dlss-4.5-research.md).)
- One distribution with one look for the three addons (Neural Rendering, ReShade input passthrough, Windowed mode): done inside
  LS Addon Manager.

## Building

Visual Studio 2022 Build Tools, CMake 3.20+, and the NVIDIA DLSS SDK dropped into `external/ngx`
(see `external/ngx/README.md`). Dear ImGui is fetched by CMake. From the repository root,
`powershell -File tools\build_all.ps1 -Only nr` builds it (and the two upscaler addons; the FSR one also wants AMD's runtime from
`tools\fetch_ffx_sdk.ps1`) and `tools\package.ps1` zips a release. Details, the offline test host
and the measurement harness are in [docs/building.md](docs/building.md).

## License and credits

MIT (see `LICENSE`). Third parties: the addon SDK headers of LS Addon Manager (MIT, `../../manager/sdk`),
Dear ImGui (MIT), and AMD's FidelityFX API headers (MIT, `third_party/ffx`, used by the FSR Upscaler). The NVIDIA DLSS SDK and the
DLSSNR snippet are NVIDIA's and are not part of this repository.

DLSSNR is an NVIDIA technology. This addon runs NVIDIA's model on hardware and in a way NVIDIA did
not release it for. Use it at your own risk with respect to NVIDIA's terms.
