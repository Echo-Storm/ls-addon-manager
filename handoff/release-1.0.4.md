# Release 1.0.4: **released 2026-10-21** (tag v1.0.4), a hotfix

Zip `LSAddonManager-1.0.4-x64.zip`, 175,880,967 bytes, SHA-256 4df63d9db775c724b3c93feb70b76f9fc16cd1639d8885c51147cf4da7f3e61e.

- **Why:** the owner's test folder still had test builds of the addon DLLs (deployed during development, one of them 1.0.0, the VSR one from 3 October) and the 1.0.1 to 1.0.3 Setup rule "keep a file that differs from what Setup put there" left them in place: the manager was 1.0.3, those addons were not. The owner's intent: every file of ours is always the latest; only something the person *replaced* (a vendor runtime of their own) is kept.
- **In it:** `installer.cpp` `IsVendorRuntime` (only files in an addon's `dlss`, `fsr`, `xess`, `vfx`, `runtimes` folders can be kept); tests rewritten (`installer_test.cpp`); an **OLD BUILD** chip (`AddonInfo::OlderBuild`, the DLL's `GetAddonVersion` against the manager's; card, tooltip, detail page, log; `core_test` covers it); the VSR repeat check stricter (18 pixels at 1440p, a run at least every 2 s).
- **Checked:** `run_addon_tests.ps1 -All`, `ci.ps1`, the VSR and comparison host scenarios, and an end-to-end update from the 1.0.3 Setup: a stale addon DLL was replaced (the old one in `backups`), a changed vendor runtime was kept and named.
- **Open:** board E20 (EchoFG and Lossless Scaling's FPS counter), the live checks (R1, V1, N3, E10).
