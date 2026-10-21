# The addon API: what stays stable

If you write an addon for LS Addon Manager, this page says what you can rely on. It is the promise for **API 1.x**; the guide to writing an addon is [addon-authors.md](addon-authors.md).

Status: the addon API is **1.4.0** (1.0.0 from the first release; 1.1 added `GetDispatchingContext`, 1.2 `CreateImage` and `ReleaseImage`, 1.3 the manager's recorder: `RecorderFlags`, `RecorderOffer`, `RecorderSave` and the event `EAM_EVENT_RECORDING_SAVED`, and the manager's GPU limit: `GpuLimitPercent`, `GpuLoadPercent`, 1.4 the event `EAM_EVENT_SCALING_STATUS`, `SetFpsCounterMultiplier`, `LosslessFrameGenType`, `LosslessDrawsFps`: an addon that makes frames of its own makes Lossless Scaling's frame rate counter show the rate the screen gets). Until the manager itself reaches 1.0, a serious mistake in the API may still be fixed at the cost of a break, and if that ever
happens it will be the first thing in the changelog. From manager 1.0 on, everything below is binding.

## Two version numbers

- The **manager's release number** (0.8.0 today; the one on the releases page and in `Lossless.dll`'s properties) says how far the project has come. Addons should not care about it.
- The **addon API version** (1.3.0 today; `EAM_API_VERSION_STRING` in [`version.h`](../manager/sdk/include/eam/version.h), and what `IHost::GetHostVersion()` returns as `(major << 16) | (minor << 8) | patch`)
  says what an addon can rely on. It moves on its own. `min_host_version` in `addon.json` is compared with **this** number: an addon that needs a newer API than the running manager provides is
  not loaded, and its card says why.

## The promise, for 1.x

An addon built against the API headers of any 1.x release **keeps loading and working in every later 1.y release**, without being rebuilt. Concretely:

| Part | What will not change within 1.x | What may be added |
|------|--------------------------------|-------------------|
| **Exports** (`AddonInitialize`, `AddonShutdown`, `GetAddonCapabilities`, `AddonRenderSettings`, `AddonInterceptResource`, `GetAddonName` and the other `GetAddon...`) | names, signatures, calling convention, and when the manager calls them | new *optional* exports (an addon that lacks them keeps working) |
| **`IHost`** | the order and signature of every existing call, and what each one does | new calls, only **at the end** of the interface (a new minor API version). An addon must check `GetHostVersion()` before using a call newer than the API it declares. |
| **Capability bits** (`EAM_CAP_...`) | the meaning of every existing bit | new bits |
| **Events** (`events.h`) | the id and the payload struct of every existing event | new events |
| **`addon.json`** | the meaning of every existing key | new optional keys; unknown keys are ignored |
| **The settings file** (`addons\config.json`) | an addon's settings are `addons.<id>.<key>` = text; `_enabled` is the manager's; `global` is the manager's | new manager keys under `global` and new `_`-prefixed keys under an addon |
| **Dear ImGui** | the exact commit the manager is built with (pinned in `manager/CMakeLists.txt`), because the manager and every addon share one `ImGuiContext` | nothing: the commit only changes with a new **major** API version |

Two further promises: a call that is removed or changed incompatibly means a new **major** version (2.0), announced in the changelog at least one release earlier, and the manager will refuse to load
an addon that needs a major version it does not have, with a message, instead of crashing.

## What is not part of the API

Anything not in the table is free to change in any release: the manager's own code and files, its window and tabs, the log format and file names, the diagnostics zip, the update check, the Setup program, the built-in features'
(ReShade passthrough, Windowed mode) settings and behaviour, and the widget helpers in `widgets.h` and `icons.h` (they are header-only: your addon carries its own copy, so a newer header never changes an addon that
was already built). Lossless Scaling's own behaviour is also outside it: the manager hooks Lossless Scaling's Direct3D 11 work, a Lossless Scaling update can break that, and the project tests one Lossless Scaling version at a time (see the
README for which).

## What guards it

- `eam_coretest` runs the manager's handling of addons (scanning, loading, starting, stopping, a faulting addon, `min_host_version`, the settings file) and `eam_sampletest` runs the [sample addon](../examples/SampleAddon)
  through the real manager: both are part of `tools\run_addon_tests.ps1`.
- The sample addon is written against the smallest part of the API an addon needs (exports, settings, status, a metric, the shared look) and is the first thing to break if a promise here is broken.
- The API version is only ever raised in [`version.h`](../manager/sdk/include/eam/version.h), together with a line in [`ihost.h`](../manager/sdk/include/eam/ihost.h) saying which version added each call.

## For addon authors

1. Declare the lowest API you need in `addon.json` (`"min_host_version": "1.0.0"`), and do not call anything newer without checking `GetHostVersion()`.
2. Build against the same Dear ImGui commit as the manager, as the guide says.
3. Do not depend on anything in the "not part of the API" paragraph.
