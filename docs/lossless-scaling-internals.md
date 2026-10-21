# Lossless Scaling, from the outside: what its files show

Studied 2026-10-21, for our own interest and to find better places to hook, from the owner's own copy (`Lossless_original.dll`, PE time stamp 2025-08-05, 7.5 MB; `LosslessScaling.dll`/`.exe`, the .NET window). Static reading only (headers, imports, strings, resource table, disassembly with LLVM's tools); nothing of Lossless Scaling's is copied into this repository, and the notes are our words. Nothing was run under a debugger yet.

Why it is worth knowing: Lossless Scaling is unlikely to be updated again, and this project may be what keeps it useful. The more of it we understand, the more of it our addons can build on instead of work around.

## The files

- `LosslessScaling.exe`/`.dll` (+ the .NET and WPF runtime beside them): the window, a .NET 9 WPF program. It calls the native DLL through its **11 exports**: `Init`, `UnInit`, `Activate`, `ApplySettings`, `SetDriverSettings`, `SetWindowsSettings`, `GetAdapterNames`, `GetDisplayNames`, `GetDwmRefreshRate`, `GetForegroundWindowEx`, `IsWindowsBuildAtLeast`. Our `Lossless.dll` stands in for it and forwards all but `ApplySettings`.
- `Lossless_original.dll` (what is left of the original `Lossless.dll`): 224 KB of code and 7 MB of **resources**. It loads `d3d11`, `dxgi`, `d2d1`, `DWrite`, `dwmapi`, the WinRT capture APIs (Windows Graphics Capture, plus Desktop Duplication as the other source) and NVAPI/NVIDIA driver settings. Its code is C++ with C++/WinRT.
- `config.ini` (beside it): a hidden settings file read with the mINI library. Keys found in the code, with what each seems to do:
  - `[rendering] flush`
  - `[capture] frametime_buffer_size`, `frametime_buffer_reset_multiplier`, `queue_draining_momentum` (how the capture queue is smoothed)
  - `[lsfg] ui_detection_rate`, `ui_detection_threshold`, `real_timestamp_tolerance`, `base_framerate_threshold` (the frame generator's HUD detection and timing; also seen in the code: `flow_scale`, `coef_scaler`, `coef_usm`, `iteration_delta`)
  - `[fps_counter] scale`, `color`, `font`, `offset_x`, `offset_y`, `show_captured` (the counter's size, colour, font, position and whether it shows two numbers)

## The resources: about 300 compiled shaders

All the shaders are resources of type RCDATA, numbered 101 to 400: **202 DXBC** (Direct3D 11 compute and pixel shaders: the scalers and filters) and **98 SPIR-V** (the frame generation, the same passes the Linux port `lsfg-vk` runs; see [lsfg-vk-study.md](lsfg-vk-study.md)). The manager's `ShaderHook` already replaces any of them by number. They are Lossless Scaling's, so they stay out of this repository.

## The frame rate counter (the first thing we hooked)

The counter is one object of 0x78 bytes, created when scaling starts, with a DirectWrite text format and a Direct2D target on the output; its once-a-frame function is at **rva 0x1C950**. The fields that matter (offsets from the object):

| Offset | What |
|---|---|
| +0x00 | frames captured in this second (int64) |
| +0x08 | frames **presented** in this second (int64) |
| +0x10 | the captured rate (double), worked out once a second |
| +0x18 | the milliseconds the captured frames took (double) |
| +0x20 | the presented rate (double) |
| +0x28 | the milliseconds the presented frames took; when it is at least 1000 the two rates are worked out: `rate = 1000 / (ms / count)` |

The text is `"%d / %d"` (captured, then presented) when `show_captured` is 1 (its default), else `"%d"` of the presented rate. Lossless Scaling counts the frames *it* presents, so with its frame generation off and EchoFG making the frames, it shows the real rate in both numbers.

**The hook** (`manager/src/core/fps_counter_hook.cpp`, addon API 1.4 `IHost::SetFpsCounterMultiplier`): a MinHook detour on that function multiplies the presented count by what the addon says it adds, just before the rates are worked out. So `60 / 60` becomes `60 / 120`, in Lossless Scaling's own counter, in its own font and place. It is installed only when the DLL's time stamp, size and the function's bytes match this build; anything else is left alone and EchoFG draws a label of its own.

## What else looks worth a closer look

- The capture side (Windows Graphics Capture and Desktop Duplication queues, the frame-time buffer): a place to learn what a real frame is, and when.
- `ApplySettings`/`SetDriverSettings`: what the window sends down (scaling modes, frame generation type, sharpness, VRS), so an addon could read the user's choices instead of asking again.
- The window's .NET side (`LosslessScaling.dll`) is plain managed code: its settings model and strings can be read with any IL viewer.
- Hotkeys and the scaling start/stop path (`Activate`), for an addon that wants to know when scaling begins.

## Rules we keep

Read, learn, write our own; copy nothing of Lossless Scaling's into the repository or the package; hook only builds we have checked byte for byte.
