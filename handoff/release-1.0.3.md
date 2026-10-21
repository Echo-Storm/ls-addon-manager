# Release 1.0.3: **released 2026-10-21** (tag v1.0.3): NVIDIA's DLSS runtime 310.9.16, the NIS window position re-read

Zip `LSAddonManager-1.0.3-x64.zip`, 175,875,557 bytes, SHA-256 e51acd003b8ee21e1e831efaa5be37a4d1272df1afaca8a11e2466bff5a7d754 (the release page shows the same).

- **In it:** the DLSS Upscaler's runtime is 310.9.16 (NVIDIA's signed file, byte for byte their public SDK's; checked by git object id, nothing downloaded); the NIS viewport reader re-reads its constants every 4 s (`nis_pass.cpp`); the hotkey table in the README is corrected.
- **Checked:** `run_addon_tests.ps1 -All`, `ci.ps1`, the 32 DLSS host scenarios and the 8 viewport scenarios (all passed; the NGX log shows 310.9.16 loaded), an update from the 1.0.2 Setup over a fake folder (the runtime replaced, a changed file kept and named). The rest of the host matrix ran for 1.0.1 and 1.0.2; those addons are unchanged.
- **Not checked live:** a real game on 310.9.16 (offline the picture error is identical); a window moved at the same size (the host cannot simulate it).
- **Owner's calls on the way:** U4 "useless" (then: drop the .16 in anyway), S6 closed (we do not chase driver errors), I15 closed as not planned, the Done column shows 10, a GitHub stats bar on the board page.
- **Next:** live testing (board R1, V1, N3, E10), replies to #22 and #23 when they answer.
