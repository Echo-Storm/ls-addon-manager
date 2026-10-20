# Writing an addon

An addon is a native DLL (C++17, x64) in its own folder under `addons\`, with a small `addon.json`. The SDK headers are in
[`manager/sdk/include/eam/`](../manager/sdk/include/eam); include `eam/addon_sdk.h`.

**The quickest start is the [sample addon](../examples/SampleAddon):** a complete, commented, tested addon with settings, a settings panel in the manager's look, a status line and a metric, and the
CMake file to build it. What you can rely on staying the same is in [api-compatibility.md](api-compatibility.md).

```
addons\
  MyAddon\
    addon.json       manifest
    MyAddon.dll      the addon
    icon.svg         optional, shown in the addon list: its shapes, drawn in the manager's colours at any size (or icon.png, a picture)
```

## The smallest addon

```cpp
#include <eam/addon_sdk.h>
#include "imgui.h"

static IHost* g_host = nullptr;

EAM_EXPORT void AddonInitialize(IHost* host, ImGuiContext* ctx, void* allocFunc, void* freeFunc, void* userData) {
    ImGui::SetCurrentContext(ctx);
    ImGui::SetAllocatorFunctions((ImGuiMemAllocFunc)allocFunc, (ImGuiMemFreeFunc)freeFunc, userData);
    g_host = host;
    g_host->Log(EAM_LOG_INFO, "Hello from my addon");
}

EAM_EXPORT void     AddonShutdown()          { g_host = nullptr; }
EAM_EXPORT uint32_t GetAddonCapabilities()   { return EAM_CAP_NONE; }
EAM_EXPORT const char* GetAddonName()        { return "My Addon"; }
EAM_EXPORT const char* GetAddonVersion()     { return "1.0.0"; }
EAM_EXPORT const char* GetAddonAuthor()      { return "Me"; }
EAM_EXPORT const char* GetAddonDescription() { return "Does something useful."; }
```

`addon.json`:

```json
{
    "name": "My Addon",
    "version": "1.0.0",
    "author": "Me",
    "description": "What it does, in a sentence.",
    "min_host_version": "1.0.0",
    "dependencies": [],
    "tags": ["category"]
}
```

`renamed_from` (optional) lists folder names the addon used to have: on the first start under its new name the manager moves the settings saved under an old name to
the new one, and hides the old folders. `min_host_version` is compared with the **addon API** version (below), not with the manager's release number. An addon that needs a newer
API than the running manager provides is not loaded, and its card says why. Optional keys: `"dll"` (a DLL name other than the folder's) and `"icon"` (a file name other than `icon.svg` or `icon.png`). An `icon.svg` is drawn,
not shown as a picture: the manager takes the `d="..."` of each `<path>` (and the width of the `viewBox`, 24 by default), ignores colours, and
strokes the shapes in its accent colour when the addon is on and in grey when it is off, like its own icons. Line icons on a 24 x 24 grid in
the manner of [Lucide](https://lucide.dev) fit best.

`conflicts` (optional) lists the ids of addons that cannot run beside this one, for example two that work on the same frames. It is enough for one
of the two to name the other. Turning either on in the manager turns the other off (a notice says so), and if both are on at start-up, the first
in the list stays on. Supported from manager 0.8.0; an older manager ignores the key.

`wip` (optional, `true` or `false`) marks an addon as work in progress: the manager lists it with a WIP label, greys out its switch and never
loads it. Supported from manager 0.8.0.

`enabled_by_default` (optional, `true` when absent) says whether a newly installed addon starts switched on. With `false` it arrives off, and
the person switches it on in the addon list; once switched on or off, their choice is kept. For an addon that changes something everybody
would notice, such as the upscalers, which take over Lossless Scaling's NIS scaler. Supported from manager 0.9.1; an older manager ignores
the key (the addon starts on).

## Exports

| Export | Signature | |
|--------|-----------|---|
| `AddonInitialize` | `void(IHost*, ImGuiContext*, void*, void*, void*)` | required: the host, the shared ImGui context and its allocator |
| `AddonShutdown` | `void()` | required |
| `GetAddonCapabilities` | `uint32_t()` | required: a bit mask, below |
| `AddonRenderSettings` | `void()` | draws the addon's panel (with `EAM_CAP_HAS_SETTINGS`) |
| `AddonInterceptResource` | `bool(const wchar_t*, const wchar_t*, const void**, uint32_t*)` | replace one of Lossless Scaling's built-in shaders |
| `GetAddonName` / `Version` / `Author` / `Description` | `const char*()` | shown in the manager |

Capabilities: `EAM_CAP_HAS_SETTINGS`, `EAM_CAP_REQUIRES_RESTART` (enabling or disabling needs a restart), `EAM_CAP_D3D11_DEVICE_ACCESS`
(ask for Lossless Scaling's device and context), `EAM_CAP_DISPATCH_HOOK` (pre and post `Dispatch` callbacks).

## The host interface (`IHost`)

| Call | |
|------|---|
| `Log(level, message)` | thread-safe; shows in the Logs tab and `logs\LSAddonManager.log` |
| `GetConfig(addonId, key, default)`, `SetConfig(...)`, `SaveConfig()` | your settings, stored in `addons\config.json` under your id |
| `GetHostVersion()` | the addon API version, `(major << 16) \| (minor << 8) \| patch` |
| `SubscribeEvent`, `UnsubscribeEvent`, `PublishEvent` | the event bus (events in `events.h`) |
| `GetD3D11Device()`, `GetD3D11DeviceContext()` | with `EAM_CAP_D3D11_DEVICE_ACCESS` |
| `SetPreDispatchCallback`, `SetPostDispatchCallback`, `GetCurrentComputeShader`, `GetDispatchCount`, `GetDispatchingContext` (API 1.1) | with `EAM_CAP_DISPATCH_HOOK`. The callbacks run on Lossless Scaling's render thread for each of its compute passes, on any of its devices; `GetDispatchingContext` says which. |
| `CreateImage`, `ReleaseImage` (API 1.2) | Pixels you have (RGBA8) as an image for `ImGui::Image` in your panel, made on the manager window's own device (an addon cannot reach it otherwise). Release it when it is no longer drawn. |
| `RecorderFlags`, `RecorderOffer`, `RecorderSave` (API 1.3) | The manager's recorder (Features tab, *Recording*): the last seconds of Lossless Scaling's frames, saved as a `.lsrec` file. It finds the captured frames by itself; an addon offers only the frames it alone has (the presented picture before its own is drawn, an upscaler's input). `RecorderFlags()` says whether it is on (bit 0) and records what is shown (bit 1): offer nothing while bit 0 is clear. `RecorderOffer(context, texture, source, content, tag, cropW, cropH)` copies a frame on the card before it returns; `RecorderSave(name, clear)` saves what is held under a name of your own (1 saving, 0 nothing to save, 2 a save is running). The event `EAM_EVENT_RECORDING_SAVED` (payload `EamRecordingSavedData`: the file's path) says a file was written: write what you know beside it. See `ihost.h`. |
| `SetStatus(addonId, text, level)` | one line on your card and in the status bar ("Running, model 6.6 ms"); level 0 grey, 1 green, 2 amber, 3 red; refresh it, a status not refreshed for a few seconds is hidden |
| `PublishMetric(addonId, key, value, unit)` | a number for the Performance tab; call it as often as you have a new value (the manager keeps the newest few thousand per series) |

`SetStatus` and `PublishMetric` are the newest calls, appended at the end of the interface, so an addon built against an older header is unaffected.
The addon API version is in `sdk/include/eam/version.h`; it moves on its own, not with the release number. Bump its minor number when calls are added.

## Rules that are easy to trip over

- **Match the host's Dear ImGui.** The host and every addon share one `ImGuiContext`, so the struct layout must be identical. Build against the commit
  pinned in `manager/CMakeLists.txt` (each addon's CMake fetches the same one). An addon that carries its own ImGui build must also call
  `eam::ui::InitAddonImGui()` (from `widgets.h`) once after `SetCurrentContext`: some ImGui statics live per DLL and are otherwise uninitialised
  (symptom: text wrapping in the middle of words).
- **`enabled` is yours, `_enabled` is the host's.** The host keeps an addon's on/off state as `_enabled` in that addon's config object. Use any other
  key for your own settings.
- **A static CRT is invisible to the host's crash handlers.** If your DLL links the CRT statically (`/MT`), an `abort()`, `std::terminate` or invalid
  parameter inside it does not reach the host and ends Lossless Scaling with only a Windows fast-fail event. Install your own `std::set_terminate`,
  `signal(SIGABRT, ...)` and `_set_invalid_parameter_handler` and log a backtrace. Catch exceptions on any thread you create.
- **`GetConfig` strings** stay valid for the next 512 `GetConfig` calls; copy one if you need it longer.
- **Do not block the render thread.** Callbacks the manager makes from Lossless Scaling's threads (dispatch hooks, present) should return quickly.
- **A settings panel that trips an invalid CRT parameter** is switched off for the rest of the session after the first event.

## Look and feel: `widgets.h` and `icons.h`

Header-only helpers that give a panel the same look as the manager: the palette (`eam::ui::theme`), `SectionHeader`/`SectionLabel`, `Button` and
`IconButton` with icons, `SliderFloat`/`SliderInt` with a default (double-click resets, Ctrl+click types a value, Ctrl+scroll fine-tunes), a
`LineGraph`, and SVG-path icons (`eam::ui::icons::k...`) drawn straight into ImGui's draw list, so they are crisp at any scale.

## Testing without Lossless Scaling

The manager ships an offscreen renderer (`eam_uipreview`) that draws its own tabs and any addon's panel into a picture without opening a window
(`tools\ui_preview.ps1`). The manager's own tests (`manager\tools`, including a test of the sample addon) are run by `tools\run_addon_tests.ps1`, and Neural Rendering has its own in
`addons\DLSS5NR01\tools`, run by `tools\run_hosttest_matrix.py`. Copy that approach: load your DLL in a small host that implements `IHost`, render the panel, and check the result.
