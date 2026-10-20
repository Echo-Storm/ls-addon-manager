# NVIDIA Streamline SDK (research only; not shipped)

`sdk/`: Streamline v2.14.16 (https://github.com/NVIDIA-RTX/Streamline, release `streamline-sdk-v2.14.16.zip`, 276 MB), unpacked here on 2026-10-20 under NVIDIA's licence (`sdk/license.txt`).
Used only by `tools/dlssg_probe` (a lab, never built by CI, never in a release). Ignored by git: download it again with

    gh release download v2.14.16 --repo NVIDIA-RTX/Streamline --pattern "streamline-sdk-v2.14.16.zip"

and unzip into `sdk/`. Results: docs/mfg-super-motion-study.md.
