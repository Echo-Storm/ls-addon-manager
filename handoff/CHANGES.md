# 0.9.8: what changed, where, and why

For people working on the code. The player-facing version is `CHANGELOG.md`.

## HDR games in the upscalers

- **Files:** `addons/DLSS5NR01/src/addon/scaler11.cpp`/`.h` and `runtime.cpp`.
- **How it works:**
  - The grab pass applies ToSdr (`src/engine/hdr_hlsl.h`) into a half-float frame texture for 10-bit and half-float input.
  - A place pass (`ScalerLink::PlaceHdr`, `kPlaceHlsl`) writes FromSdr into NIS's output through a UAV; 8-bit frames are copied as before.
  - `SetEncoding` gets its values from `FrameEncodingOf(pass.inFmt, chain, &white, dev)`. The chain is Lossless Scaling's swap chain, got
    from NIS's back buffer (`IDXGISurface::GetParent`), released at once so Lossless Scaling can still replace it.
  - A *Frame encoding* combo under Upscaling overrides the automatic choice.
- **Tested:** in Silent Hill f (scRGB): DLSS about 1.5 ms a frame, FSR 4 about 5 ms.
- **Host test:** `nishdr=scrgb|pq` (`[check-nishdr]`).

## The engines on threads of their own (freeze-proofing)

- **Why:** FSR 4.1.1b stopped for good inside its first dispatch on an HDR frame and froze Lossless Scaling's render thread (Metro).
- **The upscalers** (`src/engine/sr_engine.cpp`/`.h`, `scaler11.cpp`):
  - `SrEngine::Submit` queues a `Job`, and a worker thread runs `Run`.
  - `RanOk(done)` marks a frame usable. It is set before submission and taken back on failure, so there is no one-frame NIS flicker.
  - `Submitted()` and `WaitSubmitted()`: the GPU waits on Lossless Scaling's queue only for work already on the engine's queue. The
    close-pair GPU wait allows up to 3 ms on the CPU.
  - `CheckStuck` fails the engine after 20 s in a runtime. `Shutdown` abandons a stuck thread rather than hanging.
  - `ResetTracking` runs when a link numbers its frames from 1 again. The thread runs at the highest priority.
- **Neural Rendering** (`src/engine/nr_engine.cpp`/`.h`, `src/addon/bridge.cpp`):
  - the same pattern;
  - the bridge changes engine settings (Prepare, SetFlowInput) only while the thread is idle (`Busy`, with a `WaitNotBusy(2)` grace);
  - a run that could not be queued still signals "finished".
- **Stall monitor:** a thread in `runtime.cpp` (`StartStallMonitor`) logs where the render thread stopped, or that no pass came for 3 s.
  It is diagnosis only.

## Saying why nothing happens

In `runtime.cpp` (`SetScalerBlocked`, shown by `panel.cpp` and `ScalerEngineText`), the panels say when:
- the card is not NVIDIA's, named from the pass itself;
- a NIS layout cannot be followed (`NisLayoutRefused`, counted for this very pass);
- a frame format cannot be taken;
- the upscaler has no link to Lossless Scaling's device, or has not replaced a frame yet.

Neural Rendering names the card frame generation runs on (`Tappable`). The upscalers' own Enable box is gone (the manager's switch only;
a saved "off" is ignored). A "getting ready" line shows while a runtime is on its first frame (`BusyMs`).

## Before / after pictures (Ctrl+Shift+F4)

- **Capture:** `src/addon/screenshot.cpp`/`.h` has `Capture`, `Tick`, `PairBase` and `ForgetCaptures`: GPU copy now, read back later
  without waiting, PNG on a thread. `screenshot_pixels.cpp`'s `ToBgra8Sdr` saves HDR in its SDR view.
- **Upscalers:** `runtime.cpp`, `g_pairStep`. The upscaled half is taken after `Upscale`. NIS's half comes from the post-dispatch callback
  `OnPostPass`, registered in `addon.cpp` for the upscalers only. A missed half is noticed at the next NIS pass.
- **Neural Rendering:** `runtime.cpp`, `g_nrPairStep` in `Present`. It is armed before Compose (enhanced, at a present `Compose` really
  composed: `g_composedNow`). The next present is forced to original, then the compare mode is put back.
- **Files:** `<game>_<date>_NR.png` / `_original.png`, and `_DLSS` or `_FSR` / `_NIS`.

## Safer hotkeys

- **Files:** `src/addon/settings.cpp`/`.h`.
- **Defaults:** F7 before / after, F8 split, F2 / F3 sharpen, F4 pictures, F1 record, F10 look.
- **Migration:** old defaults move once (`keysVersion` 3); keys chosen by hand stay. Tested in `tools/settings_test.cpp`.

## Updater

In `manager/src/update/`:
- `updater.cpp`: `CleanOldDownloads` (at a download's start and once a day from `Tick`).
- `update_check.cpp`: `PlainNotes`, and `Status.notes` from GitHub's `body`.
- `manager/src/gui/widgets/update_offer.cpp`: the notes box.

Tests are in `manager/tools/update_test.cpp`: notes, cleanup (`SetDownloadRootForTest`), and the Setup version read from the exe itself.

## Smaller

- **Recordings:** the header's `content` field (`lsrec::kSdrView`) marks an upscaler's HDR input, which is its SDR view. `nr_lsrec`
  exports such frames as they are.
- **Addons tab search:** every typed word must match (name, author, id, description or tags).
- **Installer:** `CMakeLists.txt` reconfigures when `version.h` changes, so Setup's file version cannot go stale.
- **Docs:** README (the 0.9.8 box, the Silent Hill f before / after, troubleshooting rows, the hotkeys table), `docs/faq.md` (an
  upscalers section: why the difference is smaller than built-in DLSS: no camera jitter), both addon guides, `ROADMAP.md`. The
  screenshots were retaken at 0.9.8.
