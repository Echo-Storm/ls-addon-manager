# Notice and credits

Addon Manager for Lossless Scaling (called Echo Addon Manager up to 0.8.0) began as [LosslessProxy](https://github.com/FrankBarretta/LosslessProxy) by **FrankBarretta** (MIT, 2025).
That project's proxy DLL and its DirectX 11 and shader hooks, and the ReShade and Windowed features (they began as addons), are the foundation of this one, and
we are grateful for them. At the start of this repository about half of the manager's source lines were still that code (48% by
`git blame`, before the history was started fresh); after the addon handling, safety checks, settings file, event system, host interface and
the window code were rewritten, just under a third is (29.4% by `tools/measure_original_share.py`, which compares against the original's lines; that counts the ReShade and Windowed
features, now in `manager/src/features`, as the original's, since they began as its addons). The window, tabs, Performance, backup, install and remove, tray, shared look and the live status and metrics interface are
new. The original copyright and licence are in [LICENSE](LICENSE), which must stay with every copy, however much of it remains.

## What is in this repository, and under what terms

| Part | Author | Licence |
|------|--------|---------|
| `manager/` | Echo-Storm, on FrankBarretta's LosslessProxy | MIT, [LICENSE](LICENSE) |
| `manager/src/features` (ReShade passthrough, Windowed mode) | FrankBarretta's addons, reworked and built in by Echo-Storm | MIT, [LICENSE](LICENSE) |
| `addons/DLSS5NR01` | Echo-Storm; it began as andreiday's DLSS 5 plugin for LosslessProxy, with thanks. The same sources also build the DLSS Upscaler, the FSR Upscaler and the XeSS Upscaler | MIT, its `LICENSE` |
| `tools/` | Echo-Storm | MIT |

## Third-party code that is built in or fetched at build time

| Component | Licence | Where |
|-----------|---------|-------|
| [Dear ImGui](https://github.com/ocornut/imgui) (a pinned commit) | MIT | fetched by CMake, compiled into the manager and each addon that draws a panel |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | `manager/third_party/nlohmann` |
| [stb_image](https://github.com/nothings/stb) | public domain / MIT | `manager/third_party/stb_image.h` |
| [MinHook](https://github.com/TsudaKageyu/minhook) | BSD-2-Clause | fetched by CMake for the manager (its code hooks and the Windowed feature) |
| Icon shapes | drawn in the manner of the [Lucide](https://lucide.dev) set (ISC) | `manager/sdk/include/eam/icons.h` |
| [AMD FidelityFX API headers](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) (SDK v1.1.4, FSR 3.1.4), unchanged | MIT (AMD) | `addons/DLSS5NR01/third_party/ffx`, compiled into the upscaler addons |
| FSR 4.1.1b INT8 with the RDNA 2 fix: AMD's FSR 4 upscaler library (AMD FidelityFX Upscaler Library 4.1.1.2740), as changed by the OptiScaler team so it runs on cards AMD's own FSR 4 does not; not signed. With thanks to the OptiScaler team | AMD's FidelityFX SDK 2.x licence (binary form; `LICENSE.txt` beside it, from `addons/DLSS5NR01/third_party/ffx4`) | fetched by `tools/fetch_fsr4.ps1` (pinned SHA-256); ships in `addons/FSR3UPSC/runtimes/FSR/0dd77d9c/` with `ABOUT.txt` and `LICENSE.txt`, as the FSR Upscaler's second choice (0.9.5). A stand-in until AMD's own FSR 4 runs on every card |
| Microsoft's Direct3D 12 Agility SDK core (`D3D12Core.dll`, version 1.619.6, signed by Microsoft), unmodified | Microsoft's DirectX licence terms (`Microsoft-D3D12-LICENSE.txt` beside it; the core is on the package's list of files you may distribute) | fetched by `tools/fetch_agility_sdk.ps1` (pinned SHA-256, Microsoft's signature checked); ships in `addons/FSR3UPSC/runtimes/FSR/0dd77d9c/` beside the FSR 4 INT8 runtime only. The FSR Upscaler makes its Direct3D 12 device on it when that runtime is chosen (issue #11); the shipped FSR 3.1.4 runtime does not use it |
| Intel's XeSS runtime (`libxess.dll` 2.0.2.68, from the XeSS SDK 3.0.2, signed by Intel), unmodified | Intel Simplified Software License | fetched with Intel's headers by `tools/fetch_xess_sdk.ps1` (pinned SHA-256s, Intel's signature checked; the headers are never committed); ships in `addons/XESSUPSC/xess/` with `Intel-XeSS-LICENSE.txt`, with the XeSS Upscaler (since 0.9.12) |
| AMD's FidelityFX runtime (`amd_fidelityfx_dx12.dll`, the SDK v1.1.4 prebuilt, signed by AMD), unmodified | MIT (AMD) | fetched by `tools/fetch_ffx_sdk.ps1` (pinned SHA-256, AMD's signature checked); ships in `addons/FSR3UPSC/fsr/` with `AMD-FidelityFX-LICENSE.txt`, with the FSR Upscaler (in the release since 0.9.1) |
| FSR 1's EASU edge-adaptive upscaling filter (`ffx_fsr1.h`, AMD FidelityFX SDK), ported to HLSL with direct loads in place of the gathers, in the upscalers' lean pass (`EasuTap`, `EasuSet`, `Easu` in `sr_engine.cpp`) | MIT (AMD), Copyright (C) Advanced Micro Devices, Inc. | source-level port; the licence text is in the FidelityFX SDK and `AMD-FidelityFX-LICENSE.txt` (MIT) |

## NVIDIA software in the release (not MIT)

The Neural Rendering addon uses **NVIDIA DLSS** technology. NVIDIA, the NVIDIA logo and DLSS are trademarks of NVIDIA Corporation; this
project is not affiliated with or endorsed by NVIDIA. Parts of NVIDIA's DLSS SDK, taken from NVIDIA's public repository
([NVIDIA/DLSS](https://github.com/NVIDIA/DLSS), release 310.9.1) by `tools/fetch_ngx_sdk.ps1`, are in the released files:

| What | Where in the release | Terms |
|------|----------------------|-------|
| NVIDIA's NGX SDK library (`nvsdk_ngx_s.lib`), as object code | linked into `addons/DLSS5NR01/DLSS5NR01.dll` and `nr_selftest.exe` | NVIDIA RTX SDKs licence |
| NVIDIA's DLSS runtime (`nvngx_dlss.dll` 310.9.1), unmodified | `addons/DLSS4DLAA/dlss/nvngx_dlss.dll`, with the DLSS Upscaler (in the release since 0.9.1) | NVIDIA RTX SDKs licence |
| NVIDIA's licence text | `NVIDIA-LICENSE.txt` in each of those addon folders | |
| Files of NVIDIA's Video Effects SDK (x64; `NVCVImage.dll`, `NVVideoEffects.dll`, `nvngxruntime.dll`, `cudart64_12.dll`, `libcrypto-3-x64.dll`, `nvngx_vsr.dll`, `nvVFXVideoSuperRes.dll`), unmodified, for the Video Super Resolution test addon | `addons/VSRUPSC/vfx/`, with NVIDIA's licence documents (`vfx/license/`) and `ThirdPartyLicenses.txt` | NVIDIA's Software License Agreement and product-specific terms. In the package for testing only; before a 1.0 they go behind a download option, and the terms get a proper review |

These are NVIDIA's, under NVIDIA's own licence, which comes with them. This project's MIT licence does not cover them and does not make them
redistributable on its terms: they may be passed on only as part of this application and under NVIDIA's terms. They are not in this
repository (the SDK folder is ignored by git); a build fetches them from NVIDIA with the script above, which checks each one against a pinned
SHA-256.

## Not part of this repository or the release

- **Lossless Scaling** is its author's product. This project is unofficial and is not affiliated with or endorsed by them.
- The **DLSS NR model** (`nvngx_dlssnr.dll`, needed to run DLSS 5 Neural Rendering) belongs to NVIDIA and is not a file NVIDIA offers for
  redistribution. It is never included, downloaded or linked here, and this project does not say where to find it: the person supplies their own.
