// Which addon this build is. The same sources make three addons (CMake builds them all):
//   DLSS5NR01  DLSS 5 Neural Rendering, which runs the person's own nvngx_dlssnr.dll;
//   DLSS4DLAA  DLSS Upscaler, NVIDIA DLSS Super Resolution in place of Lossless Scaling's NIS pass, with NVIDIA's runtime (NR_PRODUCT_DLAA);
//   FSR3UPSC   FSR Upscaler, AMD FidelityFX Super Resolution 3.1 in the same place, with AMD's runtime, on any graphics card (NR_PRODUCT_FSR);
//   XESSUPSC   XeSS Upscaler, Intel XeSS Super Resolution in the same place, with Intel's runtime, on any card with Shader Model 6.4 (NR_PRODUCT_XESS).
// Each has its own folder, settings and log. The upscalers take the same pass, so only one can run (their addon.json files name the
// others under "conflicts"); any of them works beside Neural Rendering.
#pragma once
#include <cstring>

namespace nr {

#if defined(NR_PRODUCT_XESS)
inline constexpr bool kScalerAddon = true;
inline constexpr bool kFsrScaler = false;
inline constexpr bool kXessScaler = true;       // ... with Intel XeSS
inline constexpr const char* kAddonId = "XESSUPSC";
inline constexpr const wchar_t* kAddonIdW = L"XESSUPSC";
inline constexpr const char* kProductName = "XeSS Upscaler";
inline constexpr const char* kUpscalerName = "XeSS";
#elif defined(NR_PRODUCT_FSR)
inline constexpr bool kScalerAddon = true;      // an upscaler in place of NIS (scaler11.h), not Neural Rendering
inline constexpr bool kFsrScaler = true;        // ... with AMD FSR (3.1, or FSR 4 from the Runtimes list) rather than NVIDIA DLSS
inline constexpr bool kXessScaler = false;
inline constexpr const char* kAddonId = "FSR3UPSC";
inline constexpr const wchar_t* kAddonIdW = L"FSR3UPSC";
inline constexpr const char* kProductName = "FSR Upscaler";
inline constexpr const char* kUpscalerName = "FSR";
#elif defined(NR_PRODUCT_DLAA)
inline constexpr bool kScalerAddon = true;
inline constexpr bool kFsrScaler = false;
inline constexpr bool kXessScaler = false;
inline constexpr const char* kAddonId = "DLSS4DLAA";
inline constexpr const wchar_t* kAddonIdW = L"DLSS4DLAA";
inline constexpr const char* kProductName = "DLSS Upscaler";
inline constexpr const char* kUpscalerName = "DLSS";
// DLSS Super Resolution in place of Lossless Scaling's NIS pass (scaler11.h). It began as DLAA on the captured frame, which in World of
// Warcraft at 4K changed nothing visible (no camera jitter, no depth), so it now does what DLSS is made for: the upscaling (and DLAA at 1:1).
// Its id is still DLSS4DLAA, so its settings stay where they are.
#else
inline constexpr bool kScalerAddon = false;
inline constexpr bool kFsrScaler = false;
inline constexpr bool kXessScaler = false;
inline constexpr const char* kAddonId = "DLSS5NR01";
inline constexpr const wchar_t* kAddonIdW = L"DLSS5NR01";
inline constexpr const char* kProductName = "DLSS 5 Neural Rendering";
inline constexpr const char* kUpscalerName = "DLSS";
#endif

// The upscalers' runtime: the folder it ships in next to the addon, its file, the Runtimes list's setting and folder
inline constexpr bool kAnyCardScaler = kFsrScaler || kXessScaler;   // runs on any card, not only NVIDIA's
inline constexpr const wchar_t* kRuntimeFolderW = kXessScaler ? L"xess" : kFsrScaler ? L"fsr" : L"dlss";
inline constexpr const wchar_t* kRuntimeFileW = kXessScaler ? L"libxess.dll" : kFsrScaler ? L"amd_fidelityfx_dx12.dll" : L"nvngx_dlss.dll";
inline constexpr const char* kRuntimeKey = kXessScaler ? "xessRuntime" : kFsrScaler ? "fsrRuntime" : "dlssRuntime";
inline constexpr const wchar_t* kRuntimeListW = kXessScaler ? L"XeSS" : kFsrScaler ? L"FSR" : L"DLSS";   // runtimes\<this>
inline constexpr const char* kRuntimeVendor = kXessScaler ? "Intel" : kFsrScaler ? "AMD" : "NVIDIA";

// Another addon of the family, by id: its name, for "... is on" messages.
inline const char* ProductNameOf(const char* id) {
    if (!id || !*id) return "";
    if (!strcmp(id, "DLSS5NR01")) return "DLSS 5 Neural Rendering";
    if (!strcmp(id, "DLSS4DLAA")) return "DLSS Upscaler";
    if (!strcmp(id, "FSR3UPSC")) return "FSR Upscaler";
    if (!strcmp(id, "XESSUPSC")) return "XeSS Upscaler";
    return id;
}

} // namespace nr
