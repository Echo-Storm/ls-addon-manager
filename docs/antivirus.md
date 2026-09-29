# "My antivirus flags the download"

Some antivirus programs (Windows Defender's cloud protection, Bitdefender) flag `LSAddonManager-<version>-x64.zip` or `LSAddonManagerSetup.exe`,
for example as `Trojan:Win32/Tecabans.STV!cl` (issue #5). What that is, and what to do.

## Why it happens

The `!cl` at the end of a Defender name means a **cloud verdict**: Defender asked Microsoft's servers, which judge a file that is new,
**unsigned** and seen on few computers as suspicious. Nothing was found in the file itself. This project fits every part of that
description, and one more: what it does is what malware does. Its `Lossless.dll` stands in for Lossless Scaling's own DLL and passes
everything on to the original (that is how a Lossless Scaling addon can exist at all), and it hooks the graphics calls of Lossless Scaling's
process. A program that replaces a DLL and hooks calls looks, to a heuristic, like the worst kind of program.

We checked the 0.9.15 files against Defender's own on-demand scanner (signatures 1.459.465.0, 2026-09-29): the zip, the Setup inside it and
`Lossless.dll` were all found clean.

## What you can do

1. **Check the download is the one we published.** Every release on the [releases page](https://github.com/Echo-Storm/ls-addon-manager/releases)
   shows a SHA-256 next to the zip. In PowerShell:
   `Get-FileHash .\LSAddonManager-0.9.15-x64.zip -Algorithm SHA256` must print the same value.
2. **Build it yourself** and scan what you built: the source is in this repository, and [the building guide](../addons/DLSS5NR01/docs/building.md)
   lists the steps. Locally built files carry no download reputation to judge, so they are usually not flagged.
3. **Tell your antivirus it is a false positive.** In Windows Security: Protection history, the entry, Actions, Allow on device. Or submit the
   file at <https://www.microsoft.com/wdsi/filesubmission> ("Software developer" / "I believe this file is not malware"); the result
   improves for everybody after a review.

## What we do about it

- Every release page has the SHA-256; the zip is made by [tools/package.ps1](../tools/package.ps1) from this repository.
- Code signing is the real fix (a signed file has a publisher that builds a reputation): it costs money and time, and is on the
  [backlog](../handoff/BACKLOG.md).
