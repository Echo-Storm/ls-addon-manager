# Tools

Scripts to build, test, look at and package LS Addon Manager. They find the repository from their own location, so run them from anywhere.
Where a script needs your Lossless Scaling folder it takes `-LsDir` (or reads the `LS_DIR` environment variable); the default is the usual Steam path.

**Rules the tools follow**

- Nothing here needs a visible window. Testing uses an offline host and an offscreen renderer, so it never takes over your screen.
- `deploy.ps1` never replaces files under a running game: it refuses while the game or Lossless Scaling runs, and backs up what it overwrites.
- The GPU test (`run_hosttest_matrix.py`) loads the GPU for about 20 seconds per scenario, three at a time. Do not run it while a game runs.

| Tool | What it does |
|---|---|
| `build_all.ps1` | Configures (first time) and builds the manager, the Neural Rendering addon, the UI preview and the offline test host, Release x64. `-Only host,nr`. |
| `run_addon_tests.ps1` | Offline tests, no game needed, in suites (core, features, sample, update, gui, installer, setupexe, nr). By default it builds and runs only the suites that the files changed since the last commit can affect, and prints only failures and the time each suite took. `-Only core,gui` picks suites, `-All` runs every one (before a release), `-List` shows what starts each. |
| `run_hosttest_matrix.py` | Runs the Neural Rendering test host through its scenarios (Neural Rendering, the DLSS and FSR Upscalers, recording, the pair) and checks the presented frame against a synthetic pattern. Three at a time (`--jobs`), printing only failures (`--verbose` for every check). `--changed`: only the scenarios for the files changed since the last commit (what `run_addon_tests.ps1` uses); `--quick`: the everyday set; no option: all but the retired ones (`RETIRED` in the script says why each is; `--retired` or `--only` runs them). About 5 minutes for all. Needs `nvngx_dlssnr.dll` (`--snippet` or `LS_DIR`). |
| `ui_preview.ps1` | Renders every tab, the addon cards and the addon panels offscreen to PNG. Set `EAM_PREVIEW_CLEAN=1` for the tidy scene used in the README. |
| `make_readme_shots.py` | Crops those renders into `docs/images`. |
| `compare_ui_renders.py` | Compares two folders of offscreen renders picture by picture (used to prove a rewrite of window code changed nothing you can see). |
| `measure_original_share.py` | How much of `manager/src` and `manager/sdk` is still the code this project started from, against a checkout of the original (see NOTICE.md). |
| `package.ps1` | Builds the release zip in `dist\` (`-Version`, `-SkipBuild`). Never packages NVIDIA's SDK or the DLSSNR snippet. |
| `fetch_xess_sdk.ps1` | Fetches Intel's XeSS SDK (release 3.0.2: headers, `libxess.dll`, licence) into `addons\DLSS5NR01\external\xess`, checked by SHA-256 and Intel's signature. Without it the XeSS Upscaler is not built. |
| `deploy.ps1` | Copies a build into a Lossless Scaling folder, with backups. `-What host\|nr\|dlaa\|fsr\|xess\|all`. |
| `analyze_ls_logs.py` | Summarises `DLSS5NR01.log`: frame-time distribution, model cost, presets applied. |
| `gpu_logger.ps1` | `nvidia-smi` once a second to a CSV; stops itself after 45 minutes. |
| `make_echo_icon.py` | Draws the Echo icon (`manager/manager-icon.ico` and `.png`), each size on its own. |
| `bmp2png.py` | BMP to PNG for a folder (needs Pillow). |
