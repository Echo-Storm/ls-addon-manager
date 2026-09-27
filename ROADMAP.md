# Road to 1.0

What 1.0 should mean: someone who has never seen this project can **install it, keep it up to date, understand what it does and does not do, and get help**, without
editing files by hand, and the promises it makes (the addon API, the settings file, safety) are stable. This page is honest about where each part stands.

Status on 2026-09-27, at version 0.9.12 (the current work is on `main`).

| # | For 1.0 | State | Notes |
|---|---------|-------|-------|
| 1 | **An installer**: install, update, repair after a Lossless Scaling update, uninstall | **Done** (0.6.0) | `LSAddonManagerSetup.exe`: one file, a small wizard, with the core (folder detection, backups, rollback, repair, uninstall) tested on fake folders and the exe tested end to end. Tried on a real install by the maintainer (the folder search, the pages, reinstall, and an update from 0.5.0 to 0.6.0, which worked); the "restart as administrator" path and Windows SmartScreen's reaction are not verified. See below. |
| 2 | **An update check** | **Done** (0.5.0) | Once a day, on by default, off in *Settings > Updates*. Since 0.9.7 it offers to download and install the release (checked against GitHub's SHA-256); nothing is downloaded unless you choose it. |
| 3 | **A frozen, documented addon API** | **Done** on `main` | The API is at 1.0.0, documented (`docs/addon-authors.md`), with a written compatibility promise (`docs/api-compatibility.md`: what will and will not change before 2.0) and a sample addon (`examples/SampleAddon`) that builds against the SDK and is tested against the real manager. |
| 4 | **Every shipped feature checked in real use** | **Needs you** | Neural Rendering, the window, the tray and hotkey, and the exit path are checked live. Not yet: ReShade passthrough (hotkey and auto-click) and Windowed mode (the virtual display after a restart) have only passed offline tests. A short live try settles both. |
| 5 | **More than one game** | **Four so far** | World of Warcraft: Forever (the beta), Fallout: New Vegas (Tale of Two Wastelands), Silent Hill f (HDR) and Metro 2033 Redux, with Neural Rendering and the DLSS and FSR Upscalers (XeSS not yet in a game). A 4:3 game and one with busy foliage would round it off. |
| 6 | **Signed files** | Not before 1.0 | Decided: the files stay unsigned until after 1.0. SmartScreen's warning and the way past it are explained in the README. |
| 7 | **Help when something goes wrong** | **Done** on `main` | A troubleshooting table in the README, a questions-and-answers page (`docs/faq.md`), the diagnostics zip and the log. More entries will come from real questions. |
| 8 | **Crash safety** | Done | Exit-path tests, isolated addon calls, the compatibility test in its own process, atomic settings writes. |
| 9 | **Model compatibility data** | **Done** on `main`, needs data | The self-test writes a shareable report (`--report`, and an *Open the compatibility report* button) and `docs/model-compatibility.md` is the table. It has one row (the maintainer's card); more come from other people's reports. |
| 11 | **A finished interface** | **Done** (0.9.4) | The header with the machine's load on every tab, a slim addon list with each addon's icon and live status, icons for the features and the plugins (addons can ship an `icon.svg`), a "What comes with it" page, and screenshots rendered from the real window. |
| 12 | **The upscalers out of preview** | **Done** (0.9.4) | No longer marked work in progress; still off until switched on. Stability, edge smoothing, settings per game and 4:3 windows since 0.9.1. |
| 13 | **Runtimes you can see and switch** | **Done** (0.9.5) | The Runtimes list under the addons: each DLSS, FSR and model file with its version and signature, a tick when loaded, and **+** to switch files while the game runs (the shipped one always first). FSR 4 comes as the FSR Upscaler's second choice. |
| 10 | **Automated builds** | **Done** on `main` (green on GitHub) | `.github/workflows/build.yml` runs `tools/ci.ps1` (manager, installer and sample addon from a clean checkout, and the tests that need no GPU) on every push and pull request. Neural Rendering needs NVIDIA's SDK and the window tests need a desktop, so those stay on the maintainer's machine. |

## The installer, in detail

What it has to do (and what it must never do):

- **Find the Lossless Scaling folder**: from a running `LosslessScaling.exe`, Steam's library folders, the usual locations and the last folder used, or let the person pick it.
  Non-Steam copies are common, so picking must always work.
- **Refuse while Lossless Scaling is running**, and say why.
- **Tell whose `Lossless.dll` is whose without loading it.** Our `Lossless.dll` carries a version resource (product "Addon Manager for Lossless Scaling"); Lossless Scaling's says "Lossless Scaling".
- **Install** by backing up, renaming the original `Lossless.dll` to `Lossless_original.dll`, putting ours in its place, and copying the addons and icons. It never touches the person's
  `config.json`, their other addons or `addons\.removed`. Everything is verified by hash afterwards, and any failure rolls the folder back to how it was.
- **Repair after a Lossless Scaling update**, which puts its own `Lossless.dll` back and removes ours: keep the new original as `Lossless_original.dll` and put ours in again.
- **Uninstall**: put the original back; leave the addons and settings unless asked.
- **Say the one thing people must do themselves**: provide their own `nvngx_dlssnr.dll` (never shipped, never downloaded), and offer to place it.
- **Be honest about being unsigned**, and be a single file.

## Done in the 0.8 interface pass

- Screenshots of the picture as it is shown, with a key and a folder of your choice (Neural Rendering's panel).
- HUD protection drawn and dragged on a snapshot of the game.
- An auto mode for Neural Rendering that keeps the model within a time budget.
- Changing the model resolution made on a thread of its own: no more stall on Lossless Scaling's render thread.
- Addon API 1.2 (images for addon panels), and two `addon.json` keys: `conflicts` and `wip`.
- The version shown moved to 0.8.

## Done in 0.9: the upscalers

- **DLSS Upscaler** and **FSR Upscaler**: NVIDIA DLSS Super Resolution or AMD FSR 3.1 in place of Lossless Scaling's NIS pass, on a
  Direct3D 12 device of their own, with DLAA / native anti-aliasing when the game already fills the screen.
- **Motion measured from the frames** (this project's own estimator), with a distrust mask where it cannot be trusted, so the upscalers work
  in any game, with frame generation on or off.
- The black screen with frame generation off found and fixed (the capture is a keyed-mutex texture that a plain copy read as black).
- Tried in World of Warcraft: Forever (1440p -> 4K, and 4K DLAA) and Fallout: New Vegas (1080p -> 4K, frame generation on and off).
- Since 0.9.1: a GPU wait instead of a repeated picture, Stability (and swaying wires kept sharp), Edge smoothing, settings per game, and
  windows of another shape than the screen (4:3 games: the upscalers read NIS's viewports).
- 0.9.4 took them out of preview. 0.9.5 added FSR 4 (the OptiScaler team's build, until AMD's own FSR 4 runs on every card: then
  AMD's signed one takes its place), DLSS model E, colour and tone controls, and FSR set up as AMD's SDK 2.3 asks. Still open:
  - **A real 4:3 game**, to confirm what the test host shows.
  - **FSR 4 in real use:** tried in real games by the maintainer on an NVIDIA card (2026-09-26): it looks good, below DLSS 4, and is
    shippable. Still open: its cost on an AMD card, and AMD's own build once it covers every card.
  - **Text and HUD.** A game's HUD is drawn into the captured frame, so the upscaler sees it; DLSS softens thin text a little (FSR 3 less).
    A game with DLSS built in draws its HUD after upscaling. Options: areas that keep NIS's picture (drawn like Neural Rendering's HUD areas),
    or finding the HUD automatically, which would be an addon of its own. On hold.

## Done since 0.9.6

- **Updating from the manager** (0.9.7): the update check offers to download the release, checks it against GitHub's SHA-256 and Setup's own
  version, and Setup waits for Lossless Scaling to close, updates it and starts it again. "Don't ask again for this release" is remembered.
- **HDR games in the upscalers**: 16-bit (scRGB) and 10-bit (HDR10) frames are upscaled in DLSS's and FSR's own HDR mode (0.9.10; 0.9.8
  and 0.9.9 went through an SDR view, which dimmed highlights) and put back in their own encoding. Seen working with FSR 4 on scRGB frames
  in a real game; the HDR mode is tested offline (a 1000-nit highlight comes back within 0.2 %), not yet in a game.
- **The XeSS Upscaler** (0.9.12): Intel XeSS Super Resolution as the third upscaler (Intel's signed runtime, any card with Shader Model 6.4),
  with the same motion estimate, HDR mode and controls. Tested offline; not yet in a game.
- **A release half the size** (0.9.12): the zip holds only Setup; `LSAddonManagerSetup.exe --extract` gives the files for installing by hand.
- **Faster tests** (0.9.9): the model scenarios three at a time and without an idle wait (the full set in about 6 minutes, it was 20); the
  everyday run takes only the scenarios for what changed; ten scenarios retired (kept, with the reason, runnable by name).
- **The engines on threads of their own.** The upscalers and Neural Rendering call NVIDIA's and AMD's code from a thread of the engine's own;
  Lossless Scaling's render thread only hands frames over. A runtime that stops responding (FSR 4.1.1b once did on its first HDR frame and
  froze Lossless Scaling) now only stops the addon, and after 20 seconds its panel says so. A step towards frame generation of our own too:
  the model is already a producer apart from Lossless Scaling's frame pacing.
- **Saying why nothing happens**: a card that is not NVIDIA's (named), a window layout the upscalers cannot follow, a frame format they
  cannot take, and the upscalers' second Enable box gone (the manager's switch is the only one).

## Ideas for after 1.0

Agreed as worth doing, in no particular order; none of them is started. They come after the hardening and optimization work toward 1.0.

- **A limiter in the Performance tab.** Limits that keep the GPU under a ceiling: load (%), temperature (°C), power (W) or graphics memory, and more than one at once ("under 80 °C *or* under 90 %",
  whichever is hit first). When one is reached the manager lowers the cost of what it controls (Neural Rendering's working scale and passes first) until the reading is back under it, and says which limit is
  active ("limiting: 83 °C"), so it never looks like unexplained stutter. Steps down and back up slowly, with a pause after each step, so it does not swing back and forth.
- **More from the auto mode.** It keeps a model time budget now (0.8.0). Room to grow: a budget per game, and "keep 20 % of the GPU free" as well as a model time.
- **Per-game profiles, applied automatically.** Neural Rendering already notices which game runs; the manager's own settings and the limiter could follow the same way.
- **A display mode per game.** Many TVs run 120 Hz only below 4K (1440p 120 Hz against 4K 60 Hz). A per-game choice (for example "World of Warcraft: Forever: 2560×1440 at 120 Hz")
  that the manager switches to when the game starts scaling and puts back when it ends, with Lossless Scaling's frame generation target to match.
- **A before/after capture.** One key saves a matching pair of screenshots with and without Neural Rendering, for comparing looks and for bug reports.
- **Frame generation of our own.** Whether AMD's FSR 3 frame interpolation or Intel's XeSS frame generation (which now runs on non-Intel
  cards too), fed with this project's motion measurement, beats Lossless
  Scaling's own frame generation. First an offline comparison on real footage (the recorder's `.lsrec` files, frames dropped and rebuilt
  and scored against the real ones); only if it clearly wins, the work of putting it in Lossless Scaling's place.
- **Translations.** The interface's text in other languages, once the text lives in one table rather than in the code.
- **A session summary.** When a game closes: average and worst frame time, peak temperature and power, how long the limiter or the auto mode was active. Real numbers for the "tested with" list.
- **A stuck-state watchdog, the rest of it.** A runtime that stops responding is caught now (the engines' own threads). Still open: frames that
  stop arriving while a game runs (Lossless Scaling itself stalled), said in the panel with an offer to restart the engine. The upscalers' stall
  monitor already writes where it stopped to the log.
- **HUD areas found automatically.** The areas are drawn on a snapshot now (0.8.0). Next: suggest likely HUD areas (parts of the picture that stay put while the scene moves,
  which the upscalers' motion estimate and LSFG's flow already show). Better as an addon of its own, which Neural Rendering and the upscalers could both use.
- **A DLSS 4.5 addon** next to Neural Rendering, for games where DLSS 5's look is not wanted. To look into first: which DLSS 4.5 features can work from what Lossless Scaling has (the captured frames and
  LSFG's optical flow, but no depth and no game motion vectors), and what NVIDIA's public SDK licence allows. Researched in
  [docs/dlss-4.5-research.md](docs/dlss-4.5-research.md). Built first as DLSS 4 DLAA on the captured frame, which changed nothing visible, then as the
  **DLSS Upscaler** in 0.9 (above), with DLSS 4.5's model M as a choice.
- **Watching: openNR** (github.com/clshortfuse/openNR). It rebuilds a DLSS NR compatibility DLL from a person's own `nvngx_dlssnr.dll`, so it runs the same model, not a better one.
  It is early, with no quality or performance claims yet. Nothing to build here: if it ever produces a working DLL, the person points Neural Rendering at it and runs **Test compatibility**.
  We never ship, host or link the DLSS NR model, or files made from it.

## Pipe dreams

Not planned, and may never be possible; kept so the idea is not lost.

- **One program with Lossless Scaling.** Lossless Scaling is closed source, so the manager works beside it rather than inside it. With its
  source, its controls could live in the manager's window and the addons could use its passes directly. Short of that: read its settings
  file to show (and, while it is closed, change) its profiles, and warn when a game's profile does not suit an addon, such as an upscaler on
  with a scaler other than NIS. Its interface is not replaced, and its files are never patched.

## Not planned for 1.0

Downloading or bundling the DLSS 5 model file, in any form. Automatic installation of updates. Support for anything other than Windows and Lossless Scaling.
