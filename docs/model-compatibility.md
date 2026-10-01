# Model compatibility

DLSS 5 Neural Rendering runs a model file, `nvngx_dlssnr.dll`, that you provide yourself (this project does not include it, download it or say where to find it). Not every build of that file runs on every
NVIDIA card or driver, and the only way to know is to try it. This page collects what people have found, so the next person can see whether their combination is known to work.

## Check your own

Open the Neural Rendering panel in the manager and press **Test compatibility** (it uses the graphics card for a few seconds, so do it before starting a game). It runs the model once, in a separate program, and adds a row that says
whether it works. Then press **Open the compatibility report**: a short text file about that test. (The report is also written by running `nr_selftest.exe --model <file> --report <file>` from the addon folder.)

The report has your graphics card, the NVIDIA driver, your Windows version, the model file's **name, version and size**, the result, and a ready-made row for the table below. It has **no folders, no user name and no file hash**: it cannot identify you or say where your file came from.

## Add what you found

Open an issue at <https://github.com/Echo-Storm/ls-addon-manager/issues> and paste the report, whether it passed or failed: a failure with its reason (`NOT_SUPPORTED`, `MODEL_INIT`, ...) is just as useful. Please do not attach the model file or a link to it.

## What has been reported

| Card | Driver | Model version | Result | Addon | Reported by |
|---|---|---|---|---|---|
| NVIDIA GeForce RTX 4070 Ti SUPER (16 GB) | 616.92 | 310.8 (158.2 MB) | PASS | 0.6.0 | the maintainer (Windows 11, Lossless Scaling 3.2.2.0, World of Warcraft: Forever) |
| NVIDIA GeForce RTX 4080 (16 GB) | 617.14 | 310.8 (158.2 MB) | FAIL NOT_SUPPORTED | 0.9.23 | theblackborz (issue #12; Windows 10.0.26200) |

## What the results mean

| Result | Meaning |
|--------|---------|
| `PASS` | The model loaded, created its feature and changed the test picture on this card. It does not prove the picture looks right in every game. |
| `NO_GPU` | No NVIDIA graphics card was found. |
| `D3D12` | A Direct3D 12 device could not be created on the card. |
| `NGX_CORE` | The NVIDIA driver's NGX core did not start: try a newer driver. |
| `HELPER` | The addon's helper DLL is missing or from another version: reinstall the addon. |
| `MODEL_LOAD` | The model file is missing or is not a usable model. |
| `MODEL_INIT` | The model loaded but refused to start. |
| `NOT_SUPPORTED` | The model created no feature: that build does not support this card. |
| `FEATURE` | The model failed to create its feature for another reason (often not enough graphics memory). |
| `EVALUATE` | The model failed when it ran. |
| `UNCHANGED` | The model ran but did not change the test picture. |

More about each is in the addon's [user guide](../addons/DLSS5NR01/docs/user-guide.md).
