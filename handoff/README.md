# Handoff (updated 2026-10-20)

Everything needed to continue on another machine, without the old conversation. **Every script is in `tools/`** (see `tools/README.md`, which also lists the test programs that live beside their code). **Read `handoff/BOARD.md` first** (every open item, what it waits on; `python tools/dev.py serve` shows it as a page; `python tools/dev.py find` / `map` search the code cheaply), then this file, `BACKLOG.md` (everything not done, and what has been read line by line), `docs/README.md` (a map of all the documentation), and `docs/own-generator-plan.md` if the work is frame generation.

## State (2026-10-20)

- **Released: 1.0.0** (2026-10-20). **1.0.3** (the DLSS runtime 310.9.16: `release-1.0.3.md`), **1.0.2** (Video Super Resolution out of prototype: `release-1.0.2.md`) and **1.0.1** were released within a day (`release-1.0.1.md`: what is in it, what was checked, what is still to check live: board R1); check `gh release list`, not the changelog, for what is out. Older notes: `release-1.0.md`, `release-next.md` (written for 0.10; kept as history).
- **EchoFG** (our own frame generator) is a prototype that ships in the package since 1.0.1 (the owner's decision, 2026-10-20: everything is in the releases, as prototypes; `NR_FRAMEGEN` defaults to ON): `tools\build_framegen_test.ps1`, then `tools\deploy.ps1 -What echofg -BuildDir build_fg`. Offline it beats FSR 3.1 on our motion estimate on real clips; the picture-at-large correction (FlowFix), half-size analysis at 4K and marks are in it. Judged offline with `nr_fgeval` (SSIM and an aligned PSNR since 2026-10-20: the plain scores understate it at 4x motion); never judged live yet beside LSFG (board E19).
- **Tests:** `tools\ci.ps1` (no GPU; what GitHub runs), `tools\run_addon_tests.ps1` (the changed suites; `-All` before a release), `python tools\run_hosttest_matrix.py --jobs 1` (the GPU scenarios: an idle machine; run whole once, on a release). After a push, watch CI (`gh run watch`).
- **Owner's decisions still standing:** no tag, release or download without the owner's yes; no third-party binaries in the repository; files stay unsigned until signing is possible (issue #5).
- **Backlog and board:** [BACKLOG.md](BACKLOG.md) and [BOARD.md](BOARD.md) (`handoff/board.json` is the source; `python tools/board_note.py ID --status S --note TEXT` edits a card). Draft replies to issues: [issue-reply-drafts.md](issue-reply-drafts.md) (all posted).

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
