# NVIDIA Optical Flow API headers (research only; not shipped)

`include/nvOpticalFlowCommon.h`, `nvOpticalFlowCuda.h`, `nvOpticalFlowD3D11.h`, `nvOpticalFlowD3D12.h`: NVIDIA's headers (SPDX MIT, "Copyright (c) 2018-2024 NVIDIA CORPORATION"),
taken on 2026-10-20 from the public copy in https://github.com/jp7677/dxvk-nvapi (`inc/nvofapi`, MIT). The official SDK (with documentation and samples) is the login-gated download at
https://developer.nvidia.com/opticalflow-sdk. The runtime is the driver's own `nvofapi64.dll` (Windows, Turing and newer); nothing from NVIDIA is shipped by this project.

Used by `tools/flowlab` (a lab, not part of any build or release). The files are ignored by git: fetch them again with

    gh api repos/jp7677/dxvk-nvapi/contents/inc/nvofapi/nvOpticalFlowD3D11.h --jq .content | base64 -d > include/nvOpticalFlowD3D11.h

(and the other three). Results: docs/optical-flow-study.md.
