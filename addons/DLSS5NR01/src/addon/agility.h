// A Direct3D 12 device on Microsoft's Agility SDK core (D3D12Core.dll) found in a folder, instead of the one in Windows. The FSR 4 INT8 runtime (the OptiScaler team's build,
// chosen in the Runtimes list) crashed on an RX 6600 XT with the Windows core while the same runtime works in OptiScaler with its FsrAgilitySDKUpgrade (issue #11), which gives
// it a device from a newer core. This makes the device through ID3D12DeviceFactory, so it touches nothing else in the process (SetSDKVersion was tried first: Windows accepts
// it and goes on with its own core once any D3D12 is loaded). The caller falls back to a plain device when this says no.
#pragma once
#include <d3d12.h>
#include <string>

namespace nr {

constexpr unsigned kAgilitySdkVersion = 619;   // the D3D12SDKVersion of the D3D12Core.dll that tools\fetch_agility_sdk.ps1 fetches (1.619.6)

// True with the device made; `say` tells what happened (for the log), also when it is false and why.
bool CreateDeviceOnAgility(const std::wstring& dir, IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** device, std::string& say);

// Whether the core in `dir` is loaded in this process now (for the test).
bool AgilityCoreLoaded(const std::wstring& dir);

}   // namespace nr
