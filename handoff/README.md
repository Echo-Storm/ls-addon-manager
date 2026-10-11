# Handoff (updated 2026-10-09)

Everything needed to continue on another machine, without the old conversation. The project's general tools are in `tools/` (see `tools/README.md`); the scripts
of the 0.9.8 session are in `scripts/`. **Read `handoff/BOARD.md` first (every open item, what it waits on; `python tools/dev.py serve` shows it as a page, `python tools/dev.py find`/`map` search the code cheaply), then, in this order: this file, [release-next.md](release-next.md) (what is unreleased and how to test it), [BACKLOG.md](BACKLOG.md)
(all open work), and `docs/own-generator-plan.md` if the work is frame generation.**

## State (2026-10-09)

- **The latest release is 0.10.0** (2026-10-11; check `gh release list`, not the changelog). **Nothing since is pushed or released**: local `main` (and the branch `claude/infallible-diffie-3f06ab`, the same
  commits) holds a large unreleased body of work, all in CHANGELOG.md "Unreleased" and listed with its test steps in [release-next.md](release-next.md). The owner has said it is not time to release
  (the version number is not decided either) and will test a build first; push, tag or release only when asked.
- **What is in it** (the headline items; the changelog has the rest): the update flow (close Lossless Scaling and install, a header badge, a daily quiet-time check), the repeat skip (pictures exactly
  like the one before are not upscaled again), the lighter-upscaling auto option, the source-cadence note (a 30 fps game seen at 60 or 120 Hz), and many fixes from three independent reviews.
- **Frame generation of our own, named EchoFG** (a prototype: CMake `NR_FRAMEGEN=OFF` in every release, off by default in a test build): our own shaders now make the frames between (`addons/DLSS5NR01/src/engine/own_synth.*`),
  level with FSR 3.1 on our motion estimate on five recordings, x2 to x9, HDR, tested on a real swap chain (`nr_fgpresent`) but **never inside Lossless Scaling**: the owner's live look is the next step.
  Build it with `tools\build_framegen_test.ps1`, deploy with `tools\deploy.ps1 -What echofg -BuildDir build_fg`. EchoFG is an addon of its own since 2026-10-11 (`ECHOFG`, `src/echofg/echofg_addon.cpp`, built only with `-DNR_FRAMEGEN=ON`; it left the FSR Upscaler, whose panel it only confused); `python tools/dev.py logs` reads `logs\ECHOFG.log`. The method, the lab, every measurement and the review fixes: `docs/own-generator-plan.md`.
- **Test games** (the owner's choice; small ones, not the huge titles): SUPERHOT, Tomb Raider 2013, DOOM 2016, Saints Row 2 (natively 30 fps), WoW for lighting; see release-next.md.
- **Tests:** `tools\ci.ps1` (no GPU; what GitHub runs), `tools\run_addon_tests.ps1` (the changed suites; `-All` before a release), `tools\run_hosttest_matrix.py` (the Neural Rendering host scenarios;
  do not run it whole repeatedly, it is costly: use `--only`). After a push, watch CI (`gh run watch`).
- **Research notes** (docs/): `own-generator-plan.md`, `lsfg-vk-study.md` and `magpie-fork-study.md` (ideas only: both are not ours to copy), `capture-card-sources.md`, `repeat-skip-spec.md`, `frame-generation-research.md`.
- **Backlog:** everything not done yet is in [BACKLOG.md](BACKLOG.md); keep it current. Draft replies to open issues are in [issue-reply-drafts.md](issue-reply-drafts.md) and are **not posted** (the owner decides).

## Scripts of the 0.9.8 session (`scripts/`)

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
