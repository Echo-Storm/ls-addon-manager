# Handoff (2026-09-26 to 27): 0.9.8

Everything needed to continue on another machine, without the old conversation: the state, what changed and where, the scripts this
session used (in `scripts/`), and the rules. The project's general tools are in `tools/` (see `tools/README.md`).

## State

- **v0.9.8 is released.**
  - Zip `LSAddonManager-0.9.8-x64.zip`, SHA-256 `bbe8947c8f076d2e869e701486c6b8292fbb23ca2ea8bea16cbf7b3f2eb560ff`, matching GitHub's digest.
  - The release notes are the changelog's 0.9.8 section.
- **The in-app update from 0.9.7 to 0.9.8 worked end to end** (tried in person). That was the first real use of the 0.9.7 updater.
- **The full test suite has not run since 0.9.7.** 0.9.8 went out on real-game testing at the owner's call (Silent Hill f in HDR, World
  of Warcraft, Metro 2033 Redux).
  - Run `tools\run_addon_tests.ps1 -All` (with `LS_DIR` set) when there is room.
  - **New matrix scenarios not yet run:** `scaler_hdr_scrgb`, `scaler_hdr_pq`, `fsr_hdr_scrgb`, `scaler_not_nvidia`.
- **Next:** 1.0 soon. Frame generation of our own comes later; the engines' own threads (below) are its groundwork.
- **Backlog:** everything not done yet is in [BACKLOG.md](BACKLOG.md); keep it current.

## What changed in 0.9.8, and where

See [CHANGES.md](CHANGES.md) for each change with its files, and why.

## Scripts this session used (`scripts/`)

| Script | What it does |
|---|---|
| `downgrade_install.ps1` | Puts an older release on a Lossless Scaling folder, to try the in-app update: uninstalls with the same-version Setup (an older Setup refuses a newer install), installs the older one, and clears the update check's last time so the check runs at the next start. Refuses while Lossless Scaling runs. |
| `reset_update_check.py` | Only the last part: removes `updates.last_check` (and `updates.skipped`) from the addons' `config.json`, with a backup first, so the daily check runs about a minute after the start. |
| `setup_obs_for_tests.ps1` | Sets OBS Studio up for recording what the addons show: a profile and scene collection "LS Addon Tests" beside the owner's own (the whole display, NVENC AV1 CQP 16, MKV, a 30 s replay buffer saved with Ctrl+Shift+F1 like the addons' recorder, into Videosossless scalingobs). `-launch` starts obs on it with the replay buffer running. refuses while obs runs. |
| `add_ls_profile.py` | Adds a Lossless Scaling profile for a game by copying an existing one (NIS, HDR support on, frame generation off, auto-scale after 5 s), with a backup of `Settings.xml`. Lossless Scaling must be closed. Used for Metro and Silent Hill f. |
| `make_readme_pair.py` | Two screenshots, the original and the enhanced, as two labelled full frames for the README (1920 wide JPEGs; the FPS counter stays). |
| `scaler_ab.py` | Runs the test host once per setting (or runtime, or code change) and prints the picture-quality numbers side by side (the FSR flags and OptiScaler comparisons were made this way). Needs the NVIDIA SDK build. |


Everything from this session is kept in this folder, not in `tools/`, so work continuing elsewhere cannot overwrite it (or be overwritten by it).

## How to work on it (rules learned the hard way)

- **Deploying:** check that the games (WoW, Fallout: New Vegas, Silent Hill f) and Lossless Scaling are closed before deploying
  (`tools\deploy.ps1 -What all -LsDir <LS folder>`). It refuses otherwise. Move files, never delete them; everything replaced goes to
  `backups`.
- **Committing and releasing:** commit locally freely; push, tag or release only when the owner asks.
- **Tests:** build plus the one relevant quick test per change (the settings, update and core tests take seconds). The full suite and the
  GPU matrix cost a lot, so batch them before a release, not after each change.
- **Never:**
  - ship, download or point to `nvngx_dlssnr.dll` (people provide their own); `nvngx_dlss.dll` ships under NVIDIA's terms;
  - call the project "LosslessProxy" (it started from that project and thanks it);
  - inject into World of Warcraft.
- **Code signing:** not before 1.0. SignPath is ruled out: the package contains NVIDIA's and AMD's proprietary runtimes.
- **The update contract:** every released manager from 0.9.7 on starts a newer Setup with `--folder <dir> --update-when-closed --restart`
  and accepts it only if its ProductName is exactly "LS Addon Manager Setup". Never change either (comments in
  `installer/src/gui/main.cpp` and `setup.rc.in` say so).
- **Hotkeys:** the addons only watch keys, so the game sees them too. Keep the defaults off F5, F6, F9 and F11 (quicksave, quickload,
  full screen).
- **Tooling quirk:** bash heredocs mangle backslashes, and `\f`, `\r` and the like become control characters. Write Python helper
  scripts to files for anything with Windows paths or C++ escapes.
- **Research:** keep it in the repo (docs, or this folder). A session's scratch folder is temporary.
