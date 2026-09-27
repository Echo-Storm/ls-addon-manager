# Changelog

## 0.9.11 (2026-09-27)

- **Recordings of the upscalers in HDR games.** Since 0.9.10 an HDR frame reaches the upscaler as light, but the recorder still marked
  such recordings as the frame's SDR view, so `nr_lsrec export` showed them wrong. They are now marked as light (a new kind in the file)
  and exported with the roll-off screenshots use. Recordings made with 0.9.10 in an HDR game with an upscaler are mislabelled.

## 0.9.10 (2026-09-27)

The DLSS and FSR Upscalers in their own HDR mode.

- **HDR games upscaled as HDR.** 0.9.9 brought highlights back from about 170 nits to 885-950 of 1000; now they come out as the game made
  them (in the test: 1000 nits in, 999 to 1001 out, with DLSS and with FSR, in scRGB and HDR10). The frame is handed to DLSS and FSR as
  light, with their HDR mode switched on, instead of in a view squeezed into SDR's range; the motion estimate still works on that view.
  The picture controls, sharpening and edge smoothing still decide in the SDR view, but only their change goes back into the light, so
  whatever they leave alone stays exactly as it was. SDR games are unchanged.

## 0.9.9 (2026-09-26)

A fix for HDR games in the DLSS and FSR Upscalers, and faster tests.

- **HDR highlights keep their brightness in the upscalers.** In 0.9.8 the upscalers took HDR frames, but bright highlights (a sun, a lamp,
  a 1000-nit sky) came out at a fraction of their brightness, about 170 nits. HDR frames are upscaled in an SDR view that rolls
  highlights off towards white, and that roll-off was so steep that DLSS's or FSR's smallest change near white undid a highlight.
  It is now logarithmic: in the test a 1000-nit highlight comes back at about 950 nits with DLSS and 885 with FSR (it was 170).
  Neural Rendering's HDR picture is unchanged: its change still fades out above the SDR white as before.
- **Tests:** the Neural Rendering and upscaler scenarios run about four times faster (the full set in about 5 minutes, it was 20), and
  everyday runs only the scenarios for the code that changed. A runtime switch in the DLSS Upscaler is tested with enough frames after
  it (DLSS starts again cold in about two seconds).

## 0.9.8 (2026-09-26)

HDR games in the DLSS and FSR Upscalers, before / after pictures, and addons that can no longer freeze Lossless Scaling: NVIDIA's and AMD's
code now runs on threads of their own. Tried in Silent Hill f (Unreal Engine 5, HDR), World of Warcraft and Metro 2033 Redux.

- **HDR games in the DLSS and FSR Upscalers.** They took 8-bit frames only, so with an HDR game NIS kept running. Now 10-bit (HDR10)
  and 16-bit float (scRGB) frames are upscaled in their SDR view, as Neural Rendering does it since 0.9.5, and the picture goes back into
  the frame's own encoding, so highlights keep their brightness. The picture controls, sharpening and edge smoothing work the same. HDR is
  read from the display Lossless Scaling's window is on (not the card's first HDR display), and a *Frame encoding* setting under
  Upscaling overrides the automatic choice. In Silent Hill f at 2560x1440 to 4K: DLSS about 1.5 ms a frame, FSR 4 about 5 ms.
- **A stuck runtime can no longer freeze Lossless Scaling.** The upscalers and Neural Rendering now call NVIDIA's and AMD's code from a
  thread of the engine's own; Lossless Scaling's render thread only hands frames over. FSR 4.1.1b once stopped for good on its first HDR
  frame and froze the picture; now NIS (or the untouched picture) carries on, the panel says when a runtime is getting ready, and after 20
  seconds it says the runtime stopped responding. The engines' threads run at high priority, so a busy game cannot delay them.
- **Before / after pictures.** Ctrl+Shift+F4 saves two PNGs of the same moment to Pictures\Lossless Scaling: in Neural Rendering the
  picture with its result (`_NR`) and without it (`_original`); in the upscalers the upscaled picture (`_DLSS` or `_FSR`) and Lossless
  Scaling's NIS (`_NIS`). One frame of the "before" shows while it is taken. Windows' own screenshots show the game's window, not Lossless
  Scaling's picture. HDR games are saved in their SDR view, highlights rolled off. Neural Rendering's single screenshot stays on its button.
- **Safer hotkeys.** The game still sees our keys (they are only watched), and a game that acts on an F-key alone did: in Silent Hill f the
  screenshot key's F11 put the game in full screen, which stopped the scaling. The defaults now keep away from F5 and F9 (quicksave and
  quickload in Bethesda's and many other games), F6 (quicksave in Source games) and F11: before / after Ctrl+Shift+**F7**, split view
  **F8**, sharpening **F2 / F3**, screenshot and before / after pictures **F4**, save the recording **F1**, the next look F10 (as before).
  Keys still on an old default move once; keys chosen by hand stay.
- **Saying why nothing happens.** The DLSS Upscaler names the card when Lossless Scaling runs on one that is not NVIDIA's, and says to set
  Lossless Scaling's Preferred GPU or use the FSR Upscaler (it used to say only "not running yet"). Neural Rendering does the same for the card
  frame generation runs on. The upscalers also say when a window layout cannot be followed (it used to read "Choose NIS", although NIS was
  chosen), when a frame format cannot be taken, when they cannot connect to Lossless Scaling's device, or have not replaced a frame yet.
  Neural Rendering's "unsupported frame format" names the format.
- **The upscalers have one switch, the manager's.** Their own Enable box is gone: two switches for one thing confused people, and an
  unticked box left the upscaler off while the manager showed it on. An old "off" saved by that box is ignored.
- **Fewer repeated pictures.** "Wait on the GPU rather than repeat a picture" works with the engine's own thread (in Silent Hill f: no
  picture shown twice in 17,000 frames), and Neural Rendering no longer leaves out a frame that comes while the one before is being handed over.
- **The update window shows what is new.** When a release is offered, its release notes appear in the window, as plain text (links and
  formatting are dropped; nothing in them can be clicked). Shown from 0.9.8 on, so for the release after it.
- **The updater cleans up after itself.** Each update left its download (about 110 MB) in the temporary folder. Downloads of versions
  already installed, and of other versions than the one being downloaded, are now removed (at a download's start and once a day).
- **Recordings** of an upscaler in an HDR game say that their frames are the SDR view, and `nr_lsrec` exports them correctly.
- **Docs:** an upscalers section in the FAQ (including why the difference is smaller than in a game with DLSS built in), troubleshooting
  for the new messages, and the guides brought up to date.

## 0.9.7 (2026-09-26)

The manager can now update itself: it offers to download, check and install a new release.

- **Updating from the manager.** When the daily check finds a new release, a window offers **Download and install**, **Not now** or
  **Don't ask again for this release**. The About tab has the same button.
  - **How the download is checked:** the zip's address is built from the release's tag, never taken from GitHub's answer. Its size and
    SHA-256 must match GitHub's listing, and the Setup inside must be LS Addon Manager Setup of that very version. Otherwise nothing is
    used.
  - **Installing:** **Install now** starts Setup with `--update-when-closed --restart`. Setup waits (Cancel is there) until Lossless
    Scaling closes, updates it with the usual backups and rollback, and starts it again (not as administrator, even when Setup had to be).
    Setup runs elevated only when the folder needs it.
- **Setup's file version always follows the release** in every build folder. A folder configured before a version bump used to keep
  the old number.

## 0.9.6 (2026-09-26)

A quick fix for the upscalers' panels, which showed Neural Rendering's messages.

- **The DLSS and FSR Upscalers no longer say "waiting for LSFG dispatches".** That is Neural Rendering's message; until the upscaler
  starts, its panel now says what it waits for: *Lossless Scaling's NIS pass (Scaling Type: NIS, the game in a window smaller than the
  screen)*. Once it runs, its own line takes over ("DLSS 2560x1440 -> 3840x2160, ... ms").
- **Technical status shows the upscaler's own engine** (starting, running, waiting for the NIS pass, or failed and why) instead of
  Neural Rendering's, which always read "not started" there.
- Why 0.9.6 and not 0.9.5.1: the update check compares three numbers, so only 0.9.6 tells people on 0.9.5 that there is an update.

## 0.9.5 (2026-09-26)

FSR 4 on any card, runtimes you can see and switch, colour and tone in the upscalers, HDR games in Neural Rendering, and a recorder
that turns a bug into a file we can replay.

- **Runtimes, at the bottom of the addon list.** Each file the addons run on is listed: the DLSS 5 model, NVIDIA's DLSS and AMD's FSR.
  - **What each line shows:** a tick when the file is loaded right now, a circle when its addon is on and waits for a game to be
    scaled, and a cross when its addon is off; then its version. "unsigned" or
    "modified" appears when the file is not as its maker signed it. Hovering shows the maker, the signer, the path and the SHA-256.
  - **Switching:** + opens the files there are for that runtime, and one click switches while the game runs. The upscaler starts again
    on the new file in about a second; Neural Rendering reloads its model.
  - **Always a way back:** "Shipped" is always the first entry.
  - **Adding a file:** "Add a file..." takes any DLL that has the functions that runtime needs and refuses others with one line. The
    file is copied into the addon's own `runtimes` folder, so updates never touch it.
  - **How addons declare them:** a `runtimes` list in `addon.json`.
- **FSR 4 in the FSR Upscaler.** OptiScaler's FSR 4.1.1b INT8 build with the RDNA 2 fix comes as a second choice in the FSR menu; AMD's
  signed FSR 3.1.4 stays the default.
  - **It runs on NVIDIA cards too:** tested on an RTX 4070 Ti.
  - **The log says what runs:** which upscaler the runtime chose and what else it holds, for example "runs 4.1.1b (it holds: 4.1.1b,
    3.1.5, 2.3.4)".
  - **Fetching it:** `tools/fetch_fsr4.ps1` fetches it and checks its SHA-256.
- **FSR, as AMD's SDK 2.3 asks:** the upscaling context is made with the non-linear colour flag (our frames are gamma-encoded) and with the
  API version header FSR 4 runtimes expect.
  - **Which FSR runs:** the panel names it, for example "FSR 4.1.1b upscales...".
  - **Stability under FSR 4:** the slider is greyed out with a note, because FSR 4 keeps its history its own way and takes none of FSR
    3.1's tuning.
  - **Tried and left out** (test host): auto exposure, and OptiScaler's tuned FSR 3.1 values. The latter put a swaying wire 10.4 levels
    off against our 8.8.
  - **FSR 4 against 3.1.4** on the sliding picture: 11.1 levels off against 14.2.
- **Vibrance, saturation, shadows, highlights, brightness, contrast and gamma in the DLSS 4 and FSR Upscalers.** They use the same maths as Neural Rendering's Picture controls
  and are kept per game. They're applied to the frame on its way to the upscaler, in the pass that already copies it, so they cost
  nothing extra. While DLSS 5 Neural Rendering is on, its Picture section is in charge and these three grey out with a note, so the tone
  is never applied twice. Matrix scenarios `scaler_tone` and `scaler_tone_nr_on`.
- **"FSR version" in the FSR Upscaler's panel** (and "DLSS version" in the DLSS 4 Upscaler's), right under the Enable switch. It is the
  same choice as the + in the manager's Runtimes list, and switching takes a second while the game runs.
- **Fixed:** the Runtimes list's lines clashed with the addon cards' ImGui IDs ("2 visible items with conflicting ID").
- **Bug sweep before 0.9.5:**
  - **Upscaler recordings:** they now record the frame as it goes to DLSS or FSR (our own copy of it). Lossless Scaling's NIS input can
    be a texture that a plain copy reads as black with frame generation off.
  - **Recorder memory:** switching the recorder off in the panel lets its memory go at once.
  - **A model file chosen with +** is no longer saved over by Neural Rendering's panel in the moment before the addon takes it.
  - **Runtimes list:** it checks what is loaded at most every two seconds, rather than opening the files on every frame.
  - **A chosen FSR or DLSS file that was moved or deleted** falls back to the shipped one, with a line in the log, instead of leaving
    no upscaler.
  - **Test host:** the compose check starts counting at the first result, so a slow first model build (NVIDIA's cold start, up to a
    second) no longer fails it.
- **Licences:** FSR 4.1.1b ships with AMD's FidelityFX SDK 2.x licence (binary form) beside it, and `NOTICE.md` credits AMD and the OptiScaler team.
- **DLSS model E** (DLSS 3's CNN model) as a third choice beside K and M: a user reports K and M soften a still picture here.
- **Tests:** matrix scenarios `fsr_runtime_switch` and `dlss_runtime_switch` switch the runtime mid-run (the host test's `nisswitch=`).

- **A recorder, in all three addons** (Recording, for bug reports and tests). It keeps the last few seconds of the frames the addon receives,
  losslessly, and saves them as a `.lsrec` file on a button or Ctrl+Shift+F5 (F12
  is the manager's own hotkey).
  - **What it records.** Neural Rendering records the game's frames before the model sees them; the upscalers record Lossless Scaling's
    frame before NIS.
  - **What they're for.** A saved file plays back through the addon on another computer (the test host's `replay=`), so a reported
    problem can be seen and fixed without the reporter's game or display. Recordings also serve as real footage for tuning and
    comparisons.
  - **How it works.** Nothing waits. Each frame is copied on the GPU into one of six staging textures, mapped a few frames later
    without waiting, compressed by background threads straight from the mapped memory, and kept in memory up to a set number of
    seconds and megabytes. A frame that comes while every copy is busy is left out and counted.
  - **Cost.** Off by default and free when off. On, it costs processor time on the background threads; a 1080p frame takes about 13 ms
    on one thread and shrinks to about a third.
  - **The file.** Our own QOI-style lossless codec, written from QOI's public-domain specification, on 4-byte units, so 8-bit, 10-bit
    and half-float frames are all lossless.
  - **Where it goes.** Files go to Videos\Lossless Scaling, or the profile's own Videos folder when Videos is synced by OneDrive (a
    recording is gigabytes).
  - **New tools.** `nr_lsrec` shows what is in a recording, exports frames as pictures, and makes a synthetic one. `nr_rectest` tests
    the codec and file. The matrix scenario `record_replay` records during a run, reads the file back and plays it through the addon.
- **Neural Rendering works on HDR games.** HDR frames used to stop it with "unsupported frame format". Now it takes 16-bit float (scRGB)
  and 10-bit (HDR10, PQ) frames as well as 8-bit ones. The model and the look controls work on the frame's SDR view: light relative to
  Windows' *SDR content brightness*, a curve that leaves everything up to 75% of white alone and rolls brighter light off smoothly, then
  sRGB. Only the change goes back into the frame, in its own encoding, so highlights above what the SDR view holds and colours outside
  Rec.709 come back as they were. The change also fades out over the top of the rolled-off range.
  - **Automatic encoding.** 8-bit frames are SDR and 16-bit float frames are scRGB. 10-bit frames are HDR10 when Windows runs that display
    in HDR, and 10-bit SDR otherwise.
  - **Override.** A new *Frame encoding* setting (Advanced: Automatic, SDR, HDR) overrides that for the 10-bit and 16-bit frames when a
    setup is decided wrongly.
  - **Status.** Technical status shows what was decided, and the log names it whenever it changes.
  - **Tests and limits.** Two new matrix scenarios, `hdr_scrgb` and `hdr_pq`, present HDR frames with a band of 1000-nit highlights. The
    picture must change, the highlights must stay as they were, and no NaN may be written. This is untested on a real HDR display so far.
  - **The upscalers** still take 8-bit frames only; their HDR is a separate step.
- **DLSS 4 Upscaler: why DLSS did not start.** The error names NGX's own reason (the feature's init result), or the driver version DLSS
  needs, and where it looked for NVIDIA's runtime. NGX's errors and warnings go into the addon's log.

## 0.9.4 (2026-09-25)

The step toward 1.0: a finished interface, the upscalers out of preview, and the documentation rewritten around what comes with it.

- **A header on every tab**: the logo, the name and the version at the left; at the right a live readout of the machine (the graphics
  card's load and memory, system memory, the processor), so its state is seen at a glance. Memory turns amber past 80% and red past 93%;
  hovering shows the card's name, temperature, power and clocks. RAM and processor are read on demand (two system calls a second at most);
  the card as before, through NVIDIA's driver library. The header stays put while a long tab scrolls.
- **A slim addon list** (about a quarter of the window, 220 to 280 px): one compact row per addon with its icon and state dot, the name in
  Segoe UI Semibold, its live status (or version and author) under it, and a smaller switch; the search box and the install buttons sit at
  its top. The detail pane shows the addon's icon and a larger name.
- **Icons.** Addons can ship an `icon.svg`: the manager draws its shapes in its own colours at any size (accent when on, grey when off).
  Ours do: a sparkle for Neural Rendering, a small frame grown into a large one for the DLSS 4 Upscaler, the same with motion lines for
  FSR 3. The Features tab's cards have icons too (a keyboard, a monitor). Core test covers the reading of an `icon.svg`.
- **Tabs read right on a panel.** The selected tab took the panel's colour, so inside the addon's pane it vanished and the other tab looked
  selected; unselected tabs now have no fill and the selected one a light fill under the accent line, on any background.
- **About: "What comes with it"**, the three addons and the manager's built-in features in a few lines each, and the AMD FidelityFX credit.
- **The DLSS 4 and FSR 3 Upscalers are out of preview**: "(WIP)" is gone from their names, manifests, panels and install notes (they still
  arrive switched off). Their panel no longer says turning one on switches Neural Rendering off: they take the NIS pass and work beside it.
  The dead `kWip` switch is gone.
- **The README rewritten** around what is in it: a section per addon with every feature, costs and which upscaler to pick, the built-in
  features, and what the manager does; new screenshots of the real window (header, list, each addon's own panel), rendered by
  `tools/ui_preview.ps1`, which now renders the window at 1100x740 with the addons' own panels loaded from their DLLs. The upscalers' guide
  corrected: the game's MSAA helps (8x made a large difference in Fallout: New Vegas); a post-process AA before the upscaler does not.
- **Setup's pictures retaken** (they still said "Echo Addon Manager 0.7.0"): Setup has a `--shot <dir>` mode that keeps its window off the
  screen, pictures the start page, installs into the (throwaway) folder given, pictures the result and closes, remembering nothing;
  `tools/setup_shots.ps1` makes the fake folder, runs it and moves the folder away afterwards. Setup's note says it was tested with
  Fallout: New Vegas too.
- Version 0.9.4 (manager, all three addons).

The work below was done after 0.9.1 and is part of 0.9.4.

- **Stability no longer smears swaying wires.** Test host `nisline=1`: a thin line swaying sideways over a still background (a wire in
  the wind), measured against the true picture near it (scenarios `fsr_line`, `fsr_line_stable`). The motion there was measured right
  (the line's block locks onto it; a search of small extra shifts for such pixels was tried and changed nothing); what blurred the line
  was stability's extra history (8.8 levels off the truth without stability, 10.2 with 0.5). Now a one-pixel ridge on moving content is
  marked for the upscaler to take from the current frame whatever the stability: 8.8 with stability too, while a still wire's shimmer is
  calmed as before and the sliding picture still follows its motion.
- **Upscalers: settings per game** (on by default). Sharpening, stability, edge smoothing, the DLSS model and the motion are kept for each
  game by its program name and come back when it takes focus (checked about once a second). Only the window being scaled counts (its
  inside is the frame's size: a chat program in front was taken for a game in the first test), and Lossless Scaling's own windows do not,
  so changes made in the panel go to the game played last. The panel lists the games with their settings, each with a Forget button. The log
  says when a game's settings come back.
- **Upscalers: Edge smoothing** (off by default), for games without anti-aliasing of their own: our own edge anti-aliasing pass (it finds
  the edge's direction and how far its step runs each way, and blends across it by the part of a pixel the true edge covers), on the
  upscaled picture before the sharpening. Tried first on the game's frame before the upscaler, where it did nothing: DLSS and FSR 3 both
  rebuild edges their own way and gave the stair steps back (test host: the smoothed frame 7.2 levels off the ideal edge, 11.4 after FSR 3).
  After the upscaler, a hard slanted edge comes out 27% closer to the ideal with FSR 3 at 1.5x (12.45 to 9.13 levels) and 35% at 1:1;
  DLSS, which smooths edges itself, hardly changes. 0.02-0.05 ms. The engine's log gives the time of the passes after the upscaler. Test
  host: `nisedge=1` (a hard slanted edge measured against the ideal one); scenarios `fsr_aliased`, `fsr_edges`, `fsr_edges_1x`,
  `fsr_edges_sharp`, `scaler_edges`.
- **Upscalers: windows of another shape than the screen** (a 4:3 game on a 16:9 screen). Lossless Scaling scales such a window into part
  of the screen, and its NIS pass then covers only that part, which the upscalers used to leave to NIS. Now they read NIS's two viewports
  from its constants (NVIDIA's NISConfig; copied once per pass shape and read back a frame later, never waiting), use them only when they
  agree with the dispatch, the textures and NIS's own scale factor, upscale the input viewport into the output viewport and leave the
  borders alone. The log gives the viewports once, or why NIS was kept. Test host: `nisvp=1` (a 4:3 frame on a 16:9 output with NIS's
  constants); scenarios `scaler_4_3`, `fsr_4_3`.
- **Upscalers: a Stability slider** (DLSS 4 and FSR 3, off by default) for shimmer on thin lines and leaves. Without the game's sub-pixel
  camera shifts a line thinner than a pixel flickers from frame to frame, and the motion measurement reported that flicker as "do not trust
  the history here", so the upscaler passed it straight through. With stability up, the distrust mask judges by the average brightness of a
  pixel's 5x5 surroundings (which flicker leaves about the same and something newly uncovered changes), and FSR 3's own tuning keys are set
  (velocity factor, shading change, accumulation per frame, disocclusion accumulation; logged on a change). Test host: on a sliding picture
  the upscaler was told to distrust 0.1% of it rather than 0.6%, and followed the slide as well as before. Scenarios `scaler_stable`,
  `fsr_stable`.
- **Fixed: switching an upscaler off while a game was scaling could freeze Lossless Scaling** (Fallout: New Vegas with frame generation,
  2026-09-24: Windows closed it as not responding). The upscaler tore itself down while still subscribed to Lossless Scaling's device events
  and holding its frame lock; the teardown waited on the GPU, and Lossless Scaling's next device event waited on that lock. Now the addon lets
  go of Lossless Scaling first (dispatch callback, device events), releases the engine's queue from any wait for a frame that never arrived,
  never waits for ever in NVIDIA's or AMD's teardown (a stuck GPU leaves it for Lossless Scaling's exit and says so), and stays loaded until
  Lossless Scaling closes once its engine has run, as Neural Rendering does. The log times the stop. The test host switches each upscaler off
  while it runs and makes a new device after it (`unload=1`, scenarios `scaler_unload`, `fsr_unload`).
- **DLSS 5 Neural Rendering works with frame generation off.** Without frame generation there is no captured frame, so the model had
  nothing to run on. Now, after 20 presents without a capture, it takes the frame Lossless Scaling presents (whatever the scaler), and the
  compose adds that same frame's result, waiting for it on the GPU: the picture and its result always match, with no sliding. The next
  frame's copy waits on the GPU for the model's run before instead of being left out, so nearly every present gets its own result (test
  host: 184 of 190). The working size is taken as for a 1920-wide frame, so the model costs what it does with frame generation on. When
  captures come again the usual path takes over (the bridge starts afresh at each switch). Panel: *Also with frame generation off*
  (`presentMode`, on). The Present hook now also goes in from Lossless Scaling's other passes. Test host: `offframes=`, scenario
  `present_mode`; `base` runs with it off to keep checking that an old result does not stay on the screen.
  - Found in the sweep and fixed before it shipped: present mode needs half a second without a capture as well as 20 presents (frame
    generation can present up to 20 frames per real one, which a count alone would take for frame generation off); the Present hook is
    tried once per device from the other passes, not on every pass (a failure would have filled the log); a presented frame whose format
    the model cannot take (HDR) says so on the status line instead of doing nothing.
  - **It no longer waits for the model in front of every frame.** Measured in a game (4K, frame generation off): Lossless Scaling's frame sat
    ~5.5 ms on the GPU waiting for its own result before it could be shown, and 20-50% of frames missed the 60 Hz refresh. Now the compose
    takes the newest result that is ready (usually the frame before's) and moves it along that frame's motion vectors, which the run hands
    over beside the result, to where the picture is now. Test host: the wait went from 4-5.5 ms to 0.02 ms per present. The old way stays as
    *Wait for each frame's own result* (`presentWait`, off); scenario `present_wait` keeps it tested.
  - **The model runs on every frame it can keep up with.** In a game at 60 fps it ran on every other frame: Lossless Scaling's GPU runs about
    a frame behind its CPU, so the run before had not even started when the next frame came, and the frame was skipped. The bridge now has
    two input textures (and two flow textures): a frame goes to the model while it is still on the one before, as long as a run takes less
    than three quarters of a frame's time (so a slower model never builds a queue and falls a frame further behind). The progress line
    counts them ("two at once"); test host: 35 of 54 runs in present mode, none skipped.
  - The progress lines count presented frames in present mode (they were written on every frame: 1.8 MB in two minutes).
- **Neural Rendering times Lossless Scaling's side on the GPU**: the hand-over copy, the compose pass, and how long Lossless Scaling's queue
  waits for the model (D3D11 timestamps, read back a few frames late, never stalling). Logged at 60 frames and every 1200.
- **Neural Rendering's model gets the motion measured from the frames** (the upscalers' estimator, run on the model's own input at its working
  size), per pixel and for the frame itself, instead of frame generation's quarter-size flow; the compose still slides the result onto
  generated frames with that flow. *The model's motion* chooses (measured, the default, or frame generation's). A working-size change
  never waits: the estimator's old textures are retired until the GPU is past them.
- **Neural Rendering's teardown cannot hang** either: the cross-waits between Lossless Scaling's queue and the model's are released before and
  after the drain, and NVIDIA's teardown is left for the exit when the GPU has not finished.
- **No more repeated pictures: a GPU wait instead.** When the upscaler has not finished a newer picture by the time Lossless Scaling needs
  one, the picture before used to be shown again: a visible hitch, 11-16% of pictures in Fallout: New Vegas with adaptive frame generation,
  even at 1080p with the GPU far from its limit (two frames close together). Now Lossless Scaling's frame waits on the GPU for the next
  picture and shows that, as a game with DLSS built in waits for DLSS; the CPU never waits. On the test host with the GPU busied like a game
  at its limit (`gpuload=`): shown twice 38% and 49% before, 0% with the wait, and fewer frames left out. It can be turned off (*Wait on
  the GPU rather than repeat a picture*); should the engine ever not finish a frame, stopping the upscaler releases the wait. A CPU wait
  was tried first and measured worse on a busy GPU (it shifted Lossless Scaling's timing); it is gone.
- **The upscaler's numbers in the panel.** The Upscaling section shows the last second: pictures a second, the share in close pairs (within
  6 ms), the share shown twice, frames not handed over, and GPU waits; the log gives each link's totals. The test host can set the gap
  between the two frames of a pair (`nisgap=`) and busy the GPU (`gpuload=`).
- **Sharpening reaches further** (the upscalers' Sharpening slider). The slider now spans 1.6 times the strength it did: what was 0.8 is
  0.5, and the top part goes past the standard CAS / RCAS maximum (the sharpening's effect is amplified, still within 0..1). In Fallout:
  New Vegas at 1440p -> 4K it took 0.8 of the old scale to look right, so 0.5 is the new default. A value saved on the old scale is
  converted once on load (the `sharpenScale` key marks it). FSR 3 keeps its own RCAS up to the old maximum, and the engine's pass adds the
  rest on top.
- **Fewer repeated frames on a busy GPU.** In New Vegas up to one frame in six showed the picture before again (a judder that also reads as
  softness), because the upscaler's GPU work queued behind the game's and was not finished when the next frame came. The engine's queue now
  has high priority, and up to two frames may be with the engine at once (two frame buffers, three pictures in turn), so a late frame is
  shown late instead of skipped. The log counts frames not handed over and pictures shown twice. On the test host's sliding picture both
  upscalers land a little closer to the ideal picture (DLSS 16.6 instead of 18.4 levels, FSR 3 13.9 instead of 14.2).

## 0.9.1 (2026-09-24): the upscalers, and a new name

**Echo Addon Manager is now Addon Manager for Lossless Scaling** (LS Addon Manager for short; the author is still Echo-Storm), and the
repository is now `Echo-Storm/ls-addon-manager` (GitHub forwards the old address). The window, tray, installer (`LSAddonManagerSetup.exe`),
release zip and log (`logs\LSAddonManager.log`) carry the new name. Nothing to do when updating: Setup recognises an install made under
the old name and offers Update as before, the folder it remembered is still found, and settings, looks and backups carry over. The old
`EchoAddonManager.log` stays in the logs folder until you delete it.

The manager and the addons now show 0.9.1. Two new addons take the place of Lossless Scaling's NIS scaler with a temporal upscaler, fed with
motion measured from the frames themselves, so they work in any game Lossless Scaling can scale, with frame generation on or off. Both are a
**preview** (work in progress, tried in World of Warcraft: Forever only): they are in the download but arrive switched off; switch one on in
the addon list. The guide is [addons/DLSS5NR01/docs/upscalers.md](addons/DLSS5NR01/docs/upscalers.md).

- **DLSS 4 Upscaler** (`DLSS4DLAA`, NVIDIA RTX cards): NVIDIA DLSS Super Resolution in place of the NIS pass. It grew out of 0.8.0's DLSS 4
  DLAA addon, which ran on the captured frame and changed nothing visible; doing the upscaling itself is what DLSS is made for.
  - Choose **NIS** as Lossless Scaling's Scaling Type and run the game in a window smaller than the screen (2560x1440 on a 4K screen is DLSS's
    quality mode, 1.5x). At the screen's own size it runs as **DLAA** (anti-aliasing only).
  - Two DLSS models: NVIDIA's default (K, DLSS 4) and M (DLSS 4.5, about twice K's cost). NVIDIA's DLSS runtime 310.9.1 comes with it.
  - Sharpening (contrast-adaptive, the FidelityFX CAS formula), since DLSS 4 has none and NIS does.
  - Measured in World of Warcraft: Forever on an RTX 4070 Ti SUPER, model K: 2560x1440 -> 3840x2160 about 2.4 ms a frame; DLAA at 3840x2160
    about 3.2 ms (both before the motion estimate got cheaper, below).
- **FSR 3 Upscaler** (`FSR3UPSC`, any DirectX 12 card: AMD, NVIDIA or Intel): AMD FidelityFX Super Resolution 3.1 in the same place, with the
  same engine and motion, and AMD's own sharpening (RCAS). AMD's prebuilt runtime (`amd_fidelityfx_dx12.dll`, FidelityFX SDK v1.1.4, MIT,
  signed by AMD) comes with it. In World of Warcraft, 2560x1440 -> 3840x2160: about 1.85 ms a frame, and text stays crisper than with DLSS.
- The two take the same pass, so only one runs at a time: turning one on in the addon list turns the other off (the FSR addon names the DLSS
  one under `conflicts`). Either works beside DLSS 5 Neural Rendering.
- The **Before / after** hotkey (Ctrl+Shift+F6) switches between the upscaler and Lossless Scaling's own NIS while you play; the hotkeys work
  only while the upscaler is actually upscaling. If the upscaler cannot run, NIS runs as usual.

**Motion measured from the frames** (`engine/flow_estimator.cpp`, this project's own code). A temporal upscaler combines several frames and
must know where every pixel was in the frame before; a game with DLSS or FSR built in tells it, Lossless Scaling does not, and anything moving
smeared. The engine now compares each frame with the one before: a brightness pyramid down to about 64 pixels wide, a coarse-to-fine search for
every 4x4 block (seeded from the size below, with a small cost for straying from it), a fraction of a pixel at half size, a 3x3 vector median,
and a last step where every pixel picks the best of its own block's vector, the three nearest blocks' and "not moving", so motion follows edges
and a HUD that stays put stays put. Where even the best vector leaves a pixel unlike the frame before (background just uncovered, effects, a
wrong estimate), a distrust mask tells the upscaler to lean on the current frame there (DLSS's bias-toward-current-colour mask, FSR's reactive
mask) instead of smearing its history. A still picture measures exactly still.
- The upscalers' panel has a **Motion** choice: measured from the frames (the default), frame generation's flow (coarser, only with frame
  generation on), or none (to compare).
- Cost, measured in World of Warcraft on an RTX 4070 Ti SUPER: about 0.35 ms a frame at 2560x1440 and 0.7 ms at 3840x2160. The log times each
  stage (pyramid, search, median, every pixel) and says what share of the picture the mask marked (0.1-0.5% while moving, none while still).
- In the test host, on an aliased picture sliding 5.37 x 2.21 pixels a frame, the estimate finds the slide to within about a tenth of a pixel,
  and the upscaled picture lands 3x (DLSS) and 2x (FSR 3) closer to the ideal picture than with no motion.

**How it runs, and what was learned getting there**
- The upscaler runs on a Direct3D 12 device of its own (`engine/sr_engine.cpp`), never on Lossless Scaling's. NVIDIA's D3D11 DLSS run on
  Lossless Scaling's own device crashed it three times, each time somewhere else.
- On Lossless Scaling's render thread the addon only reads the frame into a shared texture, signals a shared fence and copies the newest
  finished picture into the NIS pass's output. Nothing waits: the picture shown is the one the engine finished for the frame before (one frame
  of latency), and if the engine is still busy the last picture repeats. `scalerHandoff` in the addon's config keeps test variants.
- The frame is read through the NIS pass's own view of it by a small compute pass. With frame generation off, NIS reads Lossless Scaling's
  capture directly, a keyed-mutex texture shared from its capture device, and a plain copy of it came out black: the upscaler got black
  frames, and Lossless Scaling restarted its devices every few seconds (a black screen, 2026-09-24). Reading it as NIS does fixed both.
- The log names, once per device, what the NIS pass reads and writes (with frame generation off its output is the swap chain's back buffer),
  and how bright the frame the upscaler gets and the picture it makes are, so a black picture shows in the log at once.
- A NIS pass at 1:1 is taken too (DLAA, or FSR's native anti-aliasing).
- The motion estimate was made about 40% cheaper at 4K: the search compares a checkerboard half of each 8x8 and skips the search around a
  guess that already matches exactly, and every pixel tries a vector only once.

**The manager**
- A new `addon.json` key, `enabled_by_default` (`true` when absent): with `false`, a newly installed addon arrives switched off until the
  person switches it on (docs/addon-authors.md). The upscalers use it, so an update does not hand anyone's NIS scaling to a preview.

**Found in the 0.9.1 sweep and fixed**
- The upscalers kept Neural Rendering's split-view, next-look and screenshot hotkeys, which do nothing there, and *next look* could apply a
  look saved for Neural Rendering, overwriting the upscaler's sharpening. In the upscalers only Before / after and the sharpening keys act
  now, and their Compare and hotkeys section shows just those (upscaled or NIS).
- A *Reset history* asked for while the upscaler's engine was still busy with the frame before was dropped; it now goes with the next frame.
- A fresh upscaler started with sharpening off (Neural Rendering's default), so it looked softer next to NIS, which sharpens: the upscalers
  start at 0.3 (also after *Restore defaults*).
- The FSR 3 Upscaler's status line, starting message and Performance-tab metric said "DLSS"; they name the upscaler now (`fsr_ms`).
- The master switch's tip in Neural Rendering and the upscalers still said the two exclude each other; `package.ps1` named NVIDIA's fetch
  script when AMD's runtime was missing.

**Tools and tests**
- `tools/fetch_ffx_sdk.ps1` fetches AMD's runtime from AMD's repository, pinned to FidelityFX SDK v1.1.4 and checked against a SHA-256 and
  AMD's Authenticode signature; the FidelityFX API headers (MIT) are in `addons/DLSS5NR01/third_party/ffx`. NOTICE.md lists AMD's code.
- `tools/deploy.ps1 -What fsr` deploys the FSR 3 Upscaler; `package.ps1` knows it (as work in progress); the addon test run builds it.
- The test host has a fake NIS pass (it paints its output magenta, which the upscaler must replace), frame generation off (a BGRA8 frame, no
  flow), any frame size and scale (`nisW`, `nisH`, `nisScale`, for 4K and 1:1), and a sliding aliased picture (`nismove=1`) measured against
  the ideal picture. The matrix runs both upscalers on still and moving pictures, with and without motion, and DLAA at 4K; the everyday
  quick set now includes the FSR 3 Upscaler.

## 0.8.0 (2026-09-24): the interface pass

The manager and Neural Rendering now show 0.8.0.

- **HUD protection is drawn on a picture of the game** (Neural Rendering's panel, Keep the HUD untouched). The areas were typed in as numbers before, which was cumbersome.
  - **Take a snapshot** puts the game's picture, as it is shown, in the panel.
  - **Drag on it to add an area**, drag an area to move it, drag an edge or corner to resize it, and right-click an area to remove it. The change is saved when the mouse is let go.
  - Without a snapshot the same editing works on an empty box of the frame's shape. The World of Warcraft starter layout, Clear all and edge softness stay, and the exact numbers are
    still there, folded under "Exact numbers".
  - The offline test host renders the panel with a real snapshot taken through the whole chain (the present, the GPU copy, the read-back, the manager's image).
- **A new addon: DLSS 4 DLAA, work in progress and switched off.** Tried in World of Warcraft at 4K (2026-09-24): it changed nothing visible while
  costing 3 to 4 ms of GPU time a frame, because a captured frame gives DLSS no camera jitter and no depth. It is listed with a WIP label and its
  switch in the addon list is greyed out, so it cannot be turned on (the new `"wip": true` key of `addon.json`, docs/addon-authors.md; the
  manager never loads such an addon). Its own Enable box is greyed out too (with a note saying why), it never takes the frames, it declares no conflict for now, and the release package leaves it out (`package.ps1 -IncludeWip`
  puts it in). The test host still runs it (`wipRun=1`). What it is: NVIDIA's DLSS anti-aliasing on Lossless Scaling's frames, for smoother, steadier edges
  without Neural Rendering's change of look, as its own entry in the addon list. It is built from Neural Rendering's sources, so it has the same
  pipeline and panel (the capture, Lossless Scaling's motion, the result added to every presented frame, the HUD areas, compare, screenshots,
  looks), with only the settings that apply to it. Lossless Scaling gives it no camera jitter and no depth, so it cannot add detail beyond the
  frame's own as it does in a game with DLSS; how it looks has to be judged in a game.
  - NVIDIA's DLSS runtime 310.9.1 (`nvngx_dlss.dll`) comes with it, in its `dlss` folder, under NVIDIA's licence (`NVIDIA-LICENSE.txt`):
    nothing needs to be supplied.
  - Two models: NVIDIA's default (preset K, DLSS 4) and M (DLSS 4.5's second-generation transformer). Measured in the test host at
    1920x1080 on an RTX 4070 Ti SUPER: K about 4.5 ms, M about 15 ms.
  - **Only one of the two addons runs at a time**: both would tap the same passes and add their results on top of each other. Turning one on in
    the addon list turns the other off there (a notice says so); with both on at start-up, the first in the list stays on. This is the new
    `conflicts` key of `addon.json` (docs/addon-authors.md), which any addon can use. Inside the addons a second guard does the same (the one
    not in charge clears its Enable box and lets its model go), for managers that do not know the key yet.
  - Measured on the test pattern: DLAA changes about 10% of the pixels (edges), by 0.5 levels on average, against Neural Rendering's 6.5 over
    nearly all of them. On a frame that the game has already anti-aliased, and with no camera jitter to work with, its effect is small.
  - Once an addon has hooked Present its DLL stays in memory until Lossless Scaling closes, and its hook passes straight through when it is
    off. Unloading one addon of the pair (or any addon that hooked Present before something else did) could otherwise leave a hook calling into
    freed code. A newer build of an addon that was used in the session therefore takes effect after Lossless Scaling is restarted.
  - The test host runs DLAA with both models, and both addons loaded together (the second steps aside at start, the first hands over when the
    other is turned on); the everyday quick set includes DLAA and the pair.
- **NOTICE.md corrected, and NVIDIA credited.** `DLSS5NR01.dll` and `nr_selftest.exe` link NVIDIA's NGX SDK library, so NVIDIA's object code
  is in the release; NOTICE.md said it was not. It now lists what of NVIDIA's is in the release and that NVIDIA's licence, not MIT, covers it.
  The About tab credits NVIDIA DLSS, and `NVIDIA-LICENSE.txt` ships in the addon's folder. `tools/fetch_ngx_sdk.ps1` also fetches NVIDIA's
  licence and DLSS runtime, pinned and checked like the rest.
- The Setup test hashes files through .NET: started from PowerShell 7, Windows PowerShell could not find `Get-FileHash` and the test stopped after its first install.
- The README, the roadmap, NOTICE.md and Neural Rendering's README and user guide are brought up to date (auto quality, the HUD editor, screenshots, background model making, DLSS 4 DLAA).
- The test host now saves its sample frame once the model runs steadily (the 210th present, not the 63rd), so a slower start no longer fails it.
- **Changing the model resolution no longer stalls the game.** Making the model for a new working size (a changed Model resolution, a look with
  another one, auto quality, or a new frame size) took 180 to 225 ms, measured, on Lossless Scaling's own render thread, and everything froze
  meanwhile. It is now made on a thread and a GPU queue of its own; the model pauses for that moment and its last result carries on, and the new
  model takes over between two frames. The longest hold-up of the render thread during the change is now about 6 ms, and the test host checks it.
  The panel shows how often the model was made and how long the last one took.
- **Turning frame generation off no longer leaves the last result on the screen.** Lossless Scaling keeps presenting frames, but with no frame
  generation the model has nothing new to run on, and its last result was added to every frame, standing still over a moving picture. A result
  older than half a second is no longer added. Separately, when the flow passes stop but the capture goes on, frames are no longer all dropped
  while waiting for their flow: after three frames without one they are handed over with no motion, and waiting starts again as soon as flow
  passes return. The test host and `nr_taptest` check both. (Found while measuring the change above: the host test's resolution change had
  been passing on the old result.)
- **Auto quality** (Neural Rendering's panel, Quality and performance; off by default): keeps the model within a time budget you set.
  - While the model runs over the budget, it lowers the model resolution, and raises it back toward your own setting when there is room again. Your setting is the most it uses; a
    lowest resolution you choose is the least.
  - It changes slowly, because each change rebuilds the model (a short hitch): down after 3 s over the budget, up after 10 s well under it, and only up when the model time it expects at
    the higher resolution still fits. Frames slower than 100 ms (a loading screen, a pause) are not judged. Nothing that changes the look is touched.
  - The panel shows the resolution it runs at and its recent changes; the log names each change, and the manager's Performance tab gets a `model_scale` number.
  - `nr_autotest` drives it with a simulated model: it settles within the budget without swinging back and forth, goes back up when the model gets faster, and keeps to the floor, the
    ceiling and the pauses.
- **Addon API 1.2: `IHost::CreateImage` and `ReleaseImage`.** An addon hands over RGBA pixels and gets back an image to draw in its panel with `ImGui::Image`, made on the manager
  window's own device, which an addon cannot reach otherwise. Appended at the end of the interface, so older addons are unaffected; the core test checks it. Neural Rendering now asks
  for API 1.2.
- **Screenshots** (Neural Rendering's panel, Screenshots; and Ctrl+Shift+F11 in the game, which can be changed):
  - The picture is taken as it is shown, with Neural Rendering's result, the scaling and frame generation in it. It is taken where the result is added to the presented frame, so it needs no
    ReShade.
  - It is copied on the GPU and read back a few frames later, without waiting for the GPU, then written as a PNG on a thread of its own, so the game does not stall.
  - Files are named after the game and the time, and go to `Pictures\Lossless Scaling` or a folder you choose.
  - It handles 8-bit, 10-bit and half-float frames (HDR highlights are cut, not tone-mapped).
  - The in-game corner marker is not shown for it, so it cannot end up in the picture.
  - `nr_settingstest` checks the pixel conversion.
- **The manager's window is per-monitor DPI aware in any case.** Its window thread asks for it itself, because Lossless Scaling's own manifest can stop the manager's process-wide request from
  taking effect. The text is now sharp at every display scaling, where Windows could otherwise stretch it. The GUI test runs per-monitor aware too.
- A dock beside Lossless Scaling's window was tried in this pass and taken out again: following another program's window from the outside felt too jerky to keep.
- **Notices appear at the bottom right**, above the status bar, instead of over the tabs, and long ones wrap.
- The Settings tab's boxes line up at one width.

## 0.7.9 (internal, 2026-09-23)

Not a public release (the manager still shows 0.7.0).

**Neural Rendering rewritten as our own code.** The part of it that still matches the original DLSS 5 plugin fell from 55% at the start of this work (41% before this batch) to 11%, and
what remains is mostly declarations whose names are the settings, and common idioms. The manager is at 11% too. The MIT notices of both projects stay where they are, beside ours. The
addon's author on its card is now Echo-Storm, and andreiday is thanked in NOTICE.md and the README.

- **The engine** (the model's own D3D12 device and queue) and **the forwarder** (the one module that may call the model file) are rewritten. The run is the same pass for pass: the same
  descriptors, barriers, flow scale and model passes, and every model scenario gives the same results. Also:
  - **Fixed: a GPU hang could crash Lossless Scaling.** If the GPU had not finished with a command allocator after two seconds, the engine reset it anyway, taking commands from under the
    GPU. That frame is now skipped instead; since 0.7.5 the bridge handles a run that was not queued.
  - **Fixed: upload buffers could be freed while the GPU still read them,** when waiting for the GPU timed out. They are now kept until it has finished.
  - Each of the engine's passes has its own descriptors. Before, two passes shared a slot, and two descriptors were made at every run but never used.
- **The present-time compose is rewritten**, and its shader is split into named steps: HUD areas, ghost guard, sharpening, tone, tonal ranges, colour, grain. The results are identical. The
  scratch texture a swap chain buffer needs when it cannot be written directly is now made only for such a buffer.
- **The model-side shaders are rewritten**, with identical maths.
- **`addon.cpp` is split into its parts:**
  - `runtime.cpp`: the frame path.
  - `panel.cpp`: the settings panel.
  - `tasks.cpp`: the requirements scan, the compatibility test and the file dialog.
  - `settings.cpp`: the settings and saved looks.
  - `log.cpp`: the log and the crash reports.
  - `state.h`: what they share, with which lock guards what.
- **The settings are table-driven.** Every setting of a look (its key, default and range) is written once. Before, the ranges were written three times (loading, applying a look, the panel)
  and could drift apart. Saved settings and looks load as before. A new offline test, `nr_settingstest`, checks looks as text and back, the ranges, the HUD areas, names, and the settings file
  through a stand-in host.
- **Fixed: a data race in the panel.** The reason Neural Rendering switched itself off was read on the window thread without the lock the render thread writes it under.
- Dead state removed: six variables that were written but never read.
- **`nr_harness` is retired.** It was the research tool that ran the model on a still image, and the self-test and the test host cover what it checked. It is in the history at tag v0.7.8, and
  `docs/dlssnr-knobs.md` keeps its findings.

The manager:

- **Fixed: an addon's settings panel that crashed was drawn again at every frame**, crashing each time and leaving the window's drawing half done. It is now switched off for the session after
  the first crash, as a panel that passes a bad CRT argument already was. The core test checks it with a test addon whose panel crashes.
- The About tab and the README's credits now describe what came from LosslessProxy (the idea, the addon interface, the ReShade and Windowed features) instead of the old code share.
- **README: LosslessProxy's addons load here.** The interface has only grown at the end, the exports and the older `AddonInit` name are accepted, and the events and capability bits are the
  same. The caution: an addon with a settings panel should be rebuilt against this SDK, because LosslessProxy did not pin its Dear ImGui.
- **Tests run in parallel.** `tools\run_addon_tests.ps1` builds everything once, then runs the suites that can overlap (core, sample, update, installer and the Neural Rendering model
  scenarios) all at once, and the window tests one after another beside them. It prints one line per test program. A full run takes about 7 minutes, down from 10; a change-based one about
  1.5. `-Only` with `-All` now means those suites at full depth.

## 0.7.8 (internal, 2026-09-23)

Not a public release (the manager still shows 0.7.0).

- **The manager's start-up (`main.cpp`) rewritten as our own code.**
  - One `Start`/`Stop` pair: the log, Lossless_original.dll, the one-manager-per-folder check, the settings and addons, the hooks, the built-in features, then the window thread.
  - `ApplySettings` keeps its exact signature, and the pointer to the real one now takes its type from ours, instead of a second hand-written copy of 32 parameters that could drift.
  - Its log lines say what failed and what that means ("settings will not reach Lossless Scaling").
- **The addon SDK headers rewritten** (`ihost.h`, `addon_sdk.h`; `events.h` in 0.7.7), with the same declarations in the same order (the frozen 1.0 test checks that). The documentation now says what the
  manager actually does:
  - `GetD3D11Device` is the newest device, and new ones come as scaling starts and stops;
  - capabilities are declarations, not a gate (it said the device was withheld from addons that did not ask; it never was);
  - each addon has one dispatch callback per userData, removed by itself when the addon unloads;
  - `GetConfig`'s text stays valid for hundreds of later calls.
- The one-line search box widget is gone; the Addons tab draws its search field directly.
- The original project's code is now 11% of the manager's lines (from 19.9% at the start of this work). What remains is mostly declarations whose names are the API or the manager's own interface, and
  common idioms; renaming them only to move the number is not worth the churn.

## 0.7.7 (internal, 2026-09-23)

Not a public release (the manager still shows 0.7.0).

- **Fixed (in 0.7.6, before it reached a game): the dispatch callbacks watched only the newest device.** Lossless Scaling makes two D3D11 devices within a tenth of a second when scaling
  starts, and its compute passes run on the first. The logs of 2026-09-23 show this twice. The manager now watches the contexts of every device Lossless Scaling makes, and the core test checks
  that the first device's passes still call back after a second device is made.
- **Addon API 1.1: `IHost::GetDispatchingContext()`.** Inside a dispatch callback it gives the context the pass runs on, which tells Lossless Scaling's devices apart. Like every addition, it is
  appended to the end of `IHost`, so addons built against 1.0 are unaffected.
- **Neural Rendering uses the manager's dispatch callback instead of a hook of its own.** Each of Lossless Scaling's passes now goes through one detour instead of two stacked ones (two code hooks
  on the same function can undo each other when one is removed). The addon no longer needs MinHook, and asks for API 1.1 (`min_host_version`). It declares `EAM_CAP_DISPATCH_HOOK`, and its panel
  shows the passes seen instead of a hook count. The offline test host now runs the callback the way the manager does, and every model scenario passes.
- **Fixed: two addons that both registered a dispatch callback without userData replaced each other's.** A callback was identified by its userData alone, and the SDK's default is null.
  It is now identified by the calling addon together with its userData.
- **The addon interface is now locked by a test.** The core test holds a frozen copy of the interface as released with API 1.0 and calls today's host through it, as an addon built against
  0.7.0 does. Every call must reach the right function, and the event ids and payload layouts must be unchanged.
- `events.h` rewritten, documenting each event and its payload. `SETTINGS_CHANGED` and `SHADER_INTERCEPTED` are marked as reserved: they were declared but never sent.
- **Tests: only what the change can affect.**
  - `tools\run_addon_tests.ps1` runs the suites that the files changed since the last commit can affect, and prints only failures and the time each took. `-Only core,gui` picks suites and
    `-All` runs everything; the suites are core, features, sample, update, gui, installer, setupexe and nr.
  - The Neural Rendering model scenarios have an everyday set (`run_hosttest_matrix.py --quick`: 3 scenarios, about 70 s) beside the full one (15, about 6 minutes).

## 0.7.6 (internal, 2026-09-23)

Not a public release (the manager still shows 0.7.0).

The manager's core hooks rewritten as our own code, with the bugs found on the way:

- **Fixed: addons' dispatch callbacks never ran under Lossless Scaling.** The manager patched `Dispatch` in the context's function table. Each context has its own copy of that table, and
  `SetMultithreadProtected` (which Lossless Scaling calls) swaps the copy's entries, so the patch stopped firing (checked with a test program). The same was true of `GetDispatchCount` and
  `GetCurrentComputeShader`. Neural Rendering was not affected because it hooks the code itself; the manager now does that too. It hooks each `Dispatch` implementation in d3d11.dll and runs the
  callbacks only for Lossless Scaling's context, with no cost to its dispatches while no addon has a callback.
  - The hooks are installed on the manager's window thread, before the addons load. Finding them needs D3D, which must not run inside DllMain.
  - `GetCurrentComputeShader` now asks the context during a dispatch callback, instead of hooking every `CSSetShader`; outside a callback it returns null.
  - An addon's own dispatches from inside a callback do not call it back.
- **Fixed: Windowed mode and any other MinHook user in the manager could switch each other off.** Windowed mode started MinHook for itself and, when it stopped, disabled every hook in the manager
  and shut MinHook down. MinHook is now shared with a count of users, and each part switches only its own hooks on and off.
- **Fixed, resource replacement** (the hook addons use to replace Lossless Scaling's shaders):
  - every replaced resource with a string name got the same handle, so a second one replaced the first;
  - shutting down deleted a lock that later calls still used;
  - uninstalling left the import table pointing at the hook;
  - it said "Hooks installed" even when patching had failed.

  Each replacement now gets its own handle, and the same resource with the same bytes gets the same handle again. Replacement bytes stay valid for as long as Lossless Scaling may hold them.
  Uninstalling puts the import slots back, and the log says how many of the five functions were hooked.
- **Fixed: icons in a folder with non-ASCII characters in its name did not load** (addon icons, and the window icon's PNG fallback). The image library opened files by an ANSI path. Files are now read by
  their full path, and images over 1024 pixels a side or 16 MB are refused.
- The D3D11 device hook checks what it patches. It now logs a failure instead of "installed", and never calls a missing `D3D11CreateDevice`.
- The export forwarding list is now one line per export; the exports were checked against Lossless Scaling's own DLL (all 11).
- `iat_patcher.h` is replaced by `core/hook_util` (import table lookup, delay imports, the shared MinHook).
- New core tests, on this program's own imports and on real D3D11 devices:
  - resource replacement: separate handles, reuse, pass-through and uninstall;
  - dispatch callbacks, including after multithread protection is switched on;
  - another device's context is left alone;
  - skipping a pass;
  - nothing runs after shutdown.

## 0.7.5 (internal, 2026-09-23)

Not a public release (the manager still shows 0.7.0).

- **Neural Rendering's bridge and hooks rewritten as our own code.** The bridge hands frames to the model and results back to Lossless Scaling; the hooks watch Lossless Scaling's compute passes and presents. The
  original DLSS 5 plugin is now an inspiration for these, not their source.
- **Fixed: Neural Rendering could stop for good after one failed run.** If a run was never queued (the model was not ready at that moment), the bridge still waited for it to finish. Since it never did, every
  later frame was skipped, silently. A run that is never queued is now simply forgotten.
- **Fixed: a race when the hooks are removed.** The dispatch hook read its callback twice, so removing it at the wrong moment could call a null function. Both hooks now read the callback once, atomically.
  The present hook's hit count is atomic too.
- **Old "LosslessProxy" names removed** (`lsproxy`, `LSPROXY_`, `LsProxy`, `lsp::`, `LP-icon`):
  - The addon SDK now lives in `manager/sdk/include/eam/` (`addon_sdk.h`, `widgets.h`, `icons.h` and the rest), with `EAM_*` macros, `Eam*` types and the `eam::ui` widget namespace. Only source code
    names changed: the functions an addon exports and the layout of everything passed between the manager and an addon are the same, so addons already built keep working. An addon's source needs its
    includes and names updated to match.
  - The icons are `manager-icon.ico` and `manager-icon.png`. The installer (and `tools/deploy.ps1`) moves `LP-icon.*` from an earlier install to the backups.
  - ReShade passthrough and Windowed mode keep their settings under `ReShadePassthrough` and `WindowedMode`. Settings saved under the old ids move across by themselves the first time the manager starts,
    never over settings already there. Old standalone addon folders with those names are still recognised and ignored.
  - Settings backups are marked `eam_settings_backup`; backups made by earlier versions still import.
  - The test programs are `eam_coretest`, `eam_guitest` and so on.
- New tests for all three: the settings move, old backups, and the old icon files moved aside.

## 0.7.4 (internal, 2026-09-23)

Not a public release (the manager still shows 0.7.0).

- **Neural Rendering's frame tap rewritten as our own code.** The part that watches Lossless Scaling's compute passes, finds the capture pass, and hands the model each frame with its motion was
  the largest piece still taken almost unchanged from the original DLSS 5 plugin. It now reads each pass's bound views once, and spells out the flow-pass and generated-frame rules. It also drops the
  per-pass fields nothing read any more. The saved pass text in the settings is unchanged, so existing configurations keep working.
- **Fixed: textures held after the tap went away.** The frame tap had no destructor, so the flow textures and a held frame it still owned were never released when it was destroyed (found by the new
  test: three references left where one was expected).
- **New offline test `nr_taptest`**, part of `tools/run_addon_tests.ps1`. It uses a real D3D11 device and textures shaped like LSFG 3's, with no model or NVIDIA SDK needed at run time. It checks:
  - which pass is the capture;
  - that every frame gets its own motion, and what happens when a flow pass is skipped or the flow scale changes;
  - where each present sits between real frames;
  - the old one-frame-late timing, with fresh flow off;
  - that no texture reference is left behind.
- `ROADMAP.md`: openNR noted under "watching", with what it is and is not.

## 0.7.3 (internal, 2026-09-23)

Not a public release (the manager still shows 0.7.0).

- **One manager per Lossless Scaling folder.** Lossless Scaling runs one copy of itself: starting it again loads `Lossless.dll`, hands over to the running copy and exits a moment later.
  In that moment the manager used to start completely a second time (a second tray icon, a hotkey that could not be registered, a window, the addons) and Neural Rendering reopened its log
  for writing, which wiped the running session's log and interleaved the two (seen live on 2026-09-23). The first manager in a folder now holds a named mutex; a later copy only forwards to
  Lossless Scaling and starts nothing. Neural Rendering also never wipes a log another process still has open: it writes `DLSS5NR01-<process id>.log` instead. The core test checks the guard
  with a real second process (the same folder in capitals or with a trailing separator counts as the same folder).
- First live results of the 0.7.2 motion timing, in World of Warcraft: Forever at 1440p 120 Hz: 59,904 of 60,000 frames got their own frame's motion; "ghosting seemed better".
- `ROADMAP.md`: a screenshot button and key with a chosen folder, for the 0.8 interface pass.

## 0.7.2 (internal, 2026-09-22)

Not a public release (the manager still shows 0.7.0).

- **Neural Rendering gets this frame's own motion.** The model has no game motion vectors, so it is given Lossless Scaling's optical flow instead. It used to run the moment a frame was
  captured, before LSFG had measured that frame's motion, so it got the previous frame's flow: right while motion stays steady, wrong whenever it changes (turning, starting, stopping,
  strafing), where the model's own history is then pulled the wrong way, which shows as smearing, ghosting and invented detail. The frame is now held from its capture until LSFG has issued
  that frame's finest flow pass, and handed to the model on the next dispatch, a moment later in the same frame. A frame for which LSFG runs no flow pass is dropped rather than run late.
  A new switch, **Use this frame's motion**, under *Model* (on by default), brings back the old timing for comparison. The log reports how many frames got which motion; the scenario
  matrix checks that the default gives this frame's motion and that the switch gives the old behaviour (new `flow_previous` scenario).
- Neural Rendering keeps the previous session's log as `DLSS5NR01.log.old`: a problem seen while playing is still there after Lossless Scaling restarts.
- `ROADMAP.md`: a much easier HUD protection (draw the areas over a snapshot, detect them automatically, one layout per game) joins the ideas for after 1.0.

## 0.7.1 (internal, 2026-09-22)

Not a public release: the manager still shows 0.7.0 (the shown version moves at 0.8). A bug sweep of the manager itself (the code that runs inside Lossless Scaling), with a failing test written for each bug before it was fixed, and dead code removed.

- **Loading settings from a file no longer gets undone by the restart it asks for.** The running addons still hold their old settings, and Neural Rendering writes all of its settings back when it
  shuts down, so the restart put the old saved looks, per-game looks and sliders back over the imported ones. After an import, every later write (by an addon or by the manager) now waits for the restart,
  and the Settings tab says so until then.
- **An addon's dispatch callback that faults no longer ends Lossless Scaling.** These callbacks run on Lossless Scaling's render thread and were called unguarded; a faulting one is now removed and logged.
- **Switching an addon off takes back the callbacks it left registered.** An addon that forgets to unsubscribe from events or clear its dispatch callback left pointers into its unloaded DLL; the next
  dispatch would have run freed code. Everything whose code lies in the addon's DLL is removed when it is unloaded, and the log names the addon.
- **Settings that are not valid UTF-8 no longer crash the save.** An addon storing text in the ANSI code page (a path with an accented letter, for example) made writing `config.json`, and a settings backup,
  throw; such bytes are now replaced.
- **A hand-edited `config.json` whose `addons` or `global` is not an object** no longer throws at the first write: those parts start afresh and the original file is kept as `config.json.corrupt`. An addon
  entry that is not an object is replaced when that addon writes to it.
- **One bad frame no longer takes the manager window, and Lossless Scaling, down.** An error while drawing or while handling a click (a file that could not be written, a folder that vanished) is caught,
  the half-drawn frame is unwound with Dear ImGui's error recovery, and the window carries on with a note in the log and a toast.
- Two settings imports in the same second no longer overwrite each other's copy of the previous settings.
- **ReShade passthrough could freeze the manager window when switched off.** Its watcher restyled every window of the process, the manager's own included; switching passthrough off makes
  the manager's thread wait for the watcher, and a restyle in flight at that moment waits for the manager's thread. The manager's window is now left alone (it is no ReShade overlay).
- **Windowed mode no longer wraps an older DXGI factory as a newer one.** Where `IDXGIFactory6` is missing it used to pass an `IDXGIFactory2` off as one, which would call methods that object does not
  have; it now leaves the factory unwrapped and says so in the log (every supported Windows has `IDXGIFactory6`).
- Neural Rendering no longer copies its whole settings (strings and the per-game list included) at every presented frame just to read the five hotkeys.
- `ROADMAP.md` has an "Ideas for after 1.0" list: a multi-condition GPU limiter, an auto mode for Neural Rendering, per-game profiles, a before/after capture, a session summary, a stuck-state watchdog
  and a DLSS 4.5 addon.
- **Dead code removed:** the "LS1 logic" memory patches inherited from the original project (19 hard-coded addresses for an old Lossless Scaling build, which could never be applied because the hooks are
  installed before any addon's capabilities are known; the capability bit stays in the SDK as reserved, with no effect), two DirectX 11 vtable hooks that only passed calls through
  (`CSSetShaderResources`, `CSSetUnorderedAccessViews`: one less patch in Lossless Scaling and one less hop per call), an unused `ReloadAddons`, and a per-dispatch counter in Neural Rendering that was
  never read. A duplicated test header is shared instead of copied.
- Neural Rendering and the sample addon use `FetchContent_MakeAvailable` instead of `FetchContent_Populate`, which newer CMake releases deprecate and will remove.
- New checks: the core test covers settings of the wrong shape, invalid UTF-8, the import freeze, faulting dispatch callbacks and an addon that leaves its callbacks behind (a new "leaky" test-addon mode);
  the window test makes frames throw and checks the window survives.

## 0.7.0 (2026-09-21)

Upgrading from 0.6.0: run `EchoAddonManagerSetup.exe` from the new zip and choose **Update**, or copy the new files over the old ones. Nothing to migrate. The addon API is unchanged (1.0.0).
This release is bug fixes (found by reading the code and the logs of real sessions) and the last items on the way to 1.0 that needed no one but the maintainer: a sample addon, a written promise for the
addon API, a questions-and-answers page, shareable model compatibility reports, and automated builds.

- **Setup: Lossless Scaling running was missed when the folder was written another way.** The check compared path text exactly, so a folder given with a trailing or forward slash, a `..`, or its short (8.3) name
  was not recognised as the folder of a running Lossless Scaling (the install then failed at the first file in use, and rolled back, but without saying why). Both sides are now brought to one spelling first (`CanonicalPath`).
- **Setup: an update failed on read-only files.** An installed file marked read-only could not be replaced (the whole update rolled back). The flag is lifted for the swap and put back if the swap fails or is rolled back.
- **Setup: two runs in the same second shared a backups folder** and the second overwrote the first's copies (the folder name is a time stamp to the second). A folder that exists gets a number added.
- **Setup: only one window at a time**, so a second Setup cannot start a second install in the middle of the first. A drive root as the folder (`D:\`) no longer breaks the "restart as administrator" command line.
  Choosing the folder *above* Lossless Scaling's (for example `D:\Utilities`) now uses the Lossless Scaling folder inside it when there is exactly one. The uninstall option that takes the addons out says that the addons' settings go with them.
- **The tray icon comes back after a failed re-add.** A live log showed one failed re-add after Explorer restarted lose the icon for the whole session; the window now retries every two seconds for a minute, and treats an add that
  Explorer carried out but reported as failed as done. The window test forces two failures and checks that the icon returns.
- **A sample addon** (`examples/SampleAddon`): settings, a settings panel in the manager's look, a status line and a metric, about a hundred commented lines, with its own CMake file. It is loaded, started and drawn by the real manager
  in a new offline test (`lsproxy_sampletest`, 12 checks) and built standalone by the CI script, so the addon the guide points to cannot silently stop working.
- **A written compatibility promise for the addon API** (`docs/api-compatibility.md`): for 1.x, existing exports, `IHost` calls (new ones only at the end), capability bits, events, `addon.json` keys, the settings layout and the pinned Dear ImGui commit do
  not change; what is outside the promise; what guards it.
- **A questions-and-answers page** (`docs/faq.md`) and a **model compatibility page** (`docs/model-compatibility.md`). `nr_selftest.exe --report <file>` writes a short text file to share (graphics card, driver, Windows, the model file's name,
  version and size, the result and a ready-made table row; no folders, user name or hash), the addon passes it on every test and shows an **Open the compatibility report** button, and the scenario matrix checks the report's contents.
- **Automated builds:** `tools/ci.ps1` and `.github/workflows/build.yml` build the manager, the installer and the sample addon from a clean checkout and run the tests that need no GPU (addon handling, install, update check, sample addon,
  the installer core, its file bundle, and the Setup exe's silent mode). Neural Rendering and the window tests need NVIDIA's SDK and a desktop, so they stay with the regular runner.

## 0.6.0 (2026-09-21)

Upgrading from 0.5.0: run `EchoAddonManagerSetup.exe` from the new zip (it offers **Update**), or copy the new files over the old ones as before. Nothing to migrate. The addon API is unchanged (1.0.0).
The main news is the installer; the README was rewritten around it.

- **`EchoAddonManagerSetup.exe`: an installer with a window, in one file** (`installer/`, in the zip). It finds the Lossless Scaling folder (a running copy, the last folder used, Steam
  libraries, the usual places on every drive such as `Utilities` and `Games`, or "Browse" for any other copy; a folder you pick is remembered), says what state it is in and offers the one thing that fits:
  **Install**, **Update**, **Repair** (after a Lossless Scaling update put its own `Lossless.dll` back) or **Reinstall**, plus **Uninstall** (keeping or taking out the addons).
  It refuses while Lossless Scaling runs, backs up everything it replaces, verifies by hash, undoes itself if anything fails, and never touches the person's settings, other addons or
  removed addons. When the folder needs administrator rights it offers to restart itself as administrator. Afterwards it can copy the person's own `nvngx_dlssnr.dll` into the folder
  (only when there is none; nothing is downloaded), and every page says that file is not included. The files it installs are a resource of the exe, unpacked to a temporary
  folder; a silent mode (`--silent install|uninstall|status --folder <dir>`) exists for scripts. Windows' own TaskDialog draws it. New offline tests in the runner: the file bundle
  (28 checks, including damaged and hostile bundles that must write nothing) and the exe end to end on fake folders (25 checks: silent install, reinstall, repair after an update,
  uninstall, refusals, Lossless Scaling running, and the window opening and closing by itself). The package script builds the exe with the files inside, and checks it installs them byte for byte.
- **The README was rewritten** around the installer: what it is, a three-step start with a picture of Setup, what ships, how to keep it up to date (the update check and Setup's *Update* and
  *Repair*), a troubleshooting section, and the install by hand kept as the alternative. The addon README and user guide say Setup installs the addon. `INSTALL.txt` in the zip starts with Setup.
- The build instructions no longer list build targets that were removed (`-Only host,reshade,windowed`); they say `-Only host,nr`.

## 0.5.0 (2026-09-21)

Upgrading from 0.4.1: copy the new files over the old ones. Nothing to migrate. The addon API is unchanged (1.0.0). The manager now checks GitHub once a day for a newer release: it is **on by default**
and can be turned off in *Settings > Updates* (see below; it is the only thing the manager sends over the internet, and it never downloads or installs anything).

- **An update check.** Once a day the manager asks github.com for the latest release of this project and compares its version number with yours. If there is a newer one, the status bar says
  "Update available: 0.5.0", the About tab and the Settings tab show it with an **Open the download page** button, and a notice appears once (a toast, or a balloon from the notification area
  when the window is hidden). It **never downloads or installs anything**. It is **on by default** and can be turned off in *Settings > Updates*; **Check now** (Settings and About) always works.
  It is the manager's only network access: an HTTPS request to api.github.com that GitHub sees as your IP address plus the program's name and version. The page it opens is built from the
  release's tag on this project's own address, never taken from the answer, and an oversized, slow or unreadable answer is refused. New offline test `lsproxy_updatetest` (46 checks, against a small
  server of its own on the loopback address, so it needs no internet; add `live` to also ask the real GitHub).
- **`Lossless.dll` now carries a version resource** (product "Echo Addon Manager", the release version), so Windows Explorer shows its version and the coming installer can tell it from Lossless
  Scaling's own `Lossless.dll` without loading it.
- **Toward 1.0: the installer's core, not shipped yet** (`installer/`, with `ROADMAP.md` saying what 1.0 needs). It finds the Lossless Scaling folder (a running copy, Steam libraries, the
  usual places, the last folder used), decides from the two DLLs' version resources what state the folder is in, and can install, update, repair after a Lossless Scaling update (which puts its
  own `Lossless.dll` back over ours) and uninstall, with backups of everything it replaces, verification by hash, a rollback if anything fails part-way, and no changes to the person's `config.json`,
  other addons or removed addons. It recognises earlier installs (0.4.1 and before have no version resource) by the log file name inside their `Lossless.dll`, and refuses while Lossless Scaling
  runs. A command line (`setup_cli`) drives it; the window comes next. 84 offline checks (`setup_test`, in the test runner) on fake folders, including a failure midway that must roll back.
- **The README, the addon README, the user guide and `INSTALL.txt` say what this was tested with:** World of Warcraft: Forever (the beta; it runs as `WowB.exe`), Lossless Scaling 3.2.2.0, Windows 11, RTX 4070 Ti SUPER; other
  games and setups are untested.
- Test hygiene: the features test starts each run with a fresh temporary folder (a leftover from the abrupt-exit run could make a later run with the same process id fail), and the window test
  switches the update check off so it never touches the internet.

## 0.4.1 (2026-09-21)

Upgrading from 0.4.0: copy the new files over the old ones. Nothing to migrate. This release is about how the panels look; nothing else changed.

- **Every collapsible section of the Neural Rendering panel now starts closed.** *Model*, *Quality and performance*, *Picture* and *Compare and hotkeys* used to open by themselves; now all nine
  sections (with *Keep the HUD untouched*, *Games*, *Frame detection*, *Technical status* and *Advanced*) start folded, so the panel opens as a short page: Status, Requirements, Saved looks, the
  on/off switch, and a list of sections to open. A check in the scenario runner fails if a section is ever set to open by default.
- **The open / close control of a section is now a boxed plus or minus**, not a small arrow: a white plus when the section is closed, a green minus when it is open, and it lights up when you point at it,
  so it is obvious that the section can be opened. It is the shared section widget, so the Performance tab's *All live values* section has it too.
- **Headings that cannot be closed are now the same bright green as an open section** (they were the dim green of a closed one), in the Neural Rendering panel and in the manager's own tabs
  (Settings, Features, About). Only a closed section is dim now, so at a glance bright means "this is open or always shown".

## 0.4.0 (2026-09-21)

Upgrading from 0.2.3: copy the new files over the old ones. Nothing to migrate. The version number goes from 0.2.3 to 0.4.0 on purpose: 0.3 is skipped, because
the step from 0.2.x (window rewrite, built-in features, the requirements check, the compatibility test, the reorganised panel) is bigger than a patch. The addon API is
unchanged (1.0.0), so addons built for 0.2.x still load.

- **The Neural Rendering panel is reorganised** into titled blocks with a line between them: **Status**, **Requirements**, **Saved looks (load and save your settings)**, **Neural Rendering**
  (the on/off switch) and **Settings** (the sections with the sliders). Requirements is always open now. It starts with what you have to do yourself, providing your own copy of
  `nvngx_dlssnr.dll`, in three numbered steps, then a one-line verdict, the rows, and the actions with the two main ones first (Browse, Test compatibility). Saved looks says what a look
  is and what Save, Save as new and Delete do; its list is now labelled *Saved look*. The first slider section is *Model (what it does to the picture)*, so it no longer sounds like a saved look.
- **You provide the model file yourself, said up front:** an important notice at the top of the README and of the addon's README, in the release's `INSTALL.txt`, and in the addon's description on its card.
- **Bugsweep.** A strict `/W4 /analyze` build of the manager and the addon found nothing serious; what it did find is fixed: the addon search box lower-cased text with `::tolower` on plain
  `char`s, which is undefined for non-ASCII names (an addon with an accented letter in its name); the crash logger's filter could pass a null pointer on; a `ReadFile` and a `swscanf` result were
  ignored; a process attribute list was used without a null check; and a test used the 49-day-wrapping `GetTickCount`. The analyzer's other notes are the deliberate `__except` guards
  around calls into addons (a faulting addon must not take Lossless Scaling down). The window, feature and core tests were run repeatedly (24 runs) to look for timing flakiness: none.
- **Cleanup.** The unused Dear ImGui demo source is no longer compiled into the manager; a README link that only worked through a GitHub quirk is now a plain address.
- **Settings tab:** the backup section is now *Backup and restore (save and load your settings)* with a line saying what it keeps, so it is clear that all addon settings and saved looks travel in one file.

## 0.2.3 (2026-09-21)

Upgrading from 0.2.2: copy the new files over the old ones. Nothing to migrate. `addons\DLSS5NR01` gains one file, `nr_selftest.exe`. If your security software
removes it (it is unsigned, like everything here), the *Test compatibility* button says the test program is missing; nothing else depends on it.

- **DLSS 5 Neural Rendering: a compatibility test.** A **Test compatibility** button runs the model file once, in a separate small program (`nr_selftest.exe`, new in the
  addon folder), on your graphics card, and adds a *Compatibility test* row that says whether it works there: the model must load, create its Neural Rendering feature and
  change a test picture. It runs by itself after **Browse for the model file...** has placed a file. Because it is a separate program, a model that crashes cannot take
  Lossless Scaling down. It tells you before playing that a model file cannot run on your card. Tested with the real model, and with a missing file, random bytes, no
  matching graphics card and a missing helper DLL, each of which ends with its own code and never a crash. The release zip gains `nr_selftest.exe`. The test program is started
  normally inside a job object that ends it if Lossless Scaling ends first (Windows 10 and later take the job as a start-up attribute), not started suspended and resumed: that
  is how malware starts processes it wants to tamper with, and security software watches for it.

## 0.2.2 (2026-09-21)

Upgrading from 0.2.1: copy the new files over the old ones. Nothing to migrate.

- **Fixed: a crash when Lossless Scaling exits.** Lossless Scaling can end its process without the manager shutting the addons down first, and a background thread
  that was still tracked by a `std::thread` object at that moment made Windows abort the program (exit code `0xC0000409`, an error on closing). Fixed in Neural Rendering
  (the model start-up thread, and the new requirements and file-dialog threads) and in the manager (the ReShade passthrough watcher and the Performance tab's GPU sampler).
  0.2.0 and 0.2.1 have this weakness in Neural Rendering's model start-up thread and in the manager's two threads, so it is worth updating. Found by tests that end the
  process the way Lossless Scaling can (`exitmode=abrupt`, `abrupt` and `abrupt-gpu`), which failed before the fix and are now in the test runners.
- **DLSS 5 Neural Rendering: a Requirements check** at the top of its panel: the graphics card, the NVIDIA driver's NGX core, the model file
  (`nvngx_dlssnr.dll`, with its version and size compared with the build it was tested with), the helper DLL and the engine's state, each marked OK,
  NOTE or MISSING with what to do about it, and engine failures explained in words. Nothing is loaded or downloaded to check. It does not fetch the model
  file, and this project still does not say where to get it. A **Browse for the model file...** button copies a model file you pick into the Lossless Scaling
  folder (the file that was there is moved to `backups`, never deleted).
- The release zip is now written with standard `/` path separators (earlier zips used `\`, which Windows Explorer and 7-Zip accept but other unzip tools warn about).
- **`tools\fetch_ngx_sdk.ps1`** fetches the NVIDIA DLSS SDK files needed to build Neural Rendering from NVIDIA's own public repository (pinned to a
  commit, checked by SHA-256) when you pass `-AcceptNvidiaLicense`; `-Latest` takes NVIDIA's newest commit instead, checked against NVIDIA's own git ids, and
  prints the lines to pin it. The pin is "DLSS 310.9.1 SDK", NVIDIA's newest today. The files are still not part of this repository, and never in the release zip.

## 0.2.1 (2026-09-21)

Upgrading from 0.2.0: copy the new files over the old ones. Nothing to migrate. Neural Rendering's helper DLL is now `nvngx.dll_dlss5nr01.dll`; the old
`nvngx.dll_lspnr.dll` in `addons\DLSS5NR01` is no longer used and can be deleted.

- **The manager window's code was rewritten** (window, tray icon, hotkey, dpi, the D3D11 device, the icons and the frame with its tabs and status bar), split into
  small modules under `manager/src/gui/window`, with a new offline test that drives the real window (`lsproxy_guitest`, run three ways) and checks the
  placement, hotkey text, scale limits and status text on their own. The pictures are pixel-identical to before. Fixed on the way: with the interface size
  set above or below 100 % the window was saved at a size divided by that factor and came back smaller after each close; the saved size now uses the display
  scale only. The hotkey label in the tray tip and the balloon uses the same F1 to F12 limit the hotkey itself does. The share of the manager's code that is
  still the original project's is now 29.4% (counting the ReShade and Windowed features as the original's).
- Neural Rendering's helper DLL, test tools and internal names no longer use the old `lspnr` prefix (details in the addon's changelog). Nothing changes on screen.
- **Repository clean-up.** Six of NVIDIA's DLSS SDK header files had been committed by mistake in the 0.2.0 source, although the SDK is not redistributable and
  is meant to be downloaded by whoever builds Neural Rendering (see `addons/DLSS5NR01/external/ngx/README.md`). They are removed from the repository and from its
  history, so the commit hashes after 0.1.0 changed and the `v0.2.0` tag points at the rewritten commit. The release zips never contained them. If you cloned this
  repository between the two releases, clone it again. The README now says that the SDK is needed only to build Neural Rendering, not to use the release.

## 0.2.0 (2026-09-21)

Upgrading from 0.1.0: copy the new files over the old ones, as in the install steps. Your settings carry over: Neural Rendering was renamed (its settings, looks and on/off are moved to the new
name automatically) and ReShade passthrough and Windowed mode are built in (their switches keep their place in `config.json`). The old `LSP-NeuralRender`, `LSP-ReShade` and `LSP-Windowed`
folders are ignored; you can remove them.

- **Neural Rendering is now `DLSS5NR01`** (folder, settings id, `DLSS5NR01.dll`, `DLSS5NR01.log`), no longer `LSP-NeuralRender`. New manager support for this: an addon's `addon.json` can list
  `renamed_from` folder names; the manager moves their saved settings (including on/off) to the new name, once, and hides the old folders so the same addon never runs twice. The
  saved-looks row's **Delete** button is always visible now. A compiler pass (`/W4` plus code analysis) over the manager and the addon found only trivial issues, all fixed.
- **ReShade input passthrough and Windowed mode are built into the manager** (a new **Features** tab) instead of being separate addons. Same behaviour, in
  the manager's own code and look, with the settings kept where they were (`LSP-ReShade` and `LSP-Windowed` in `config.json`). Gains: Windowed mode can be switched
  off and on again while Lossless Scaling runs (only switching it on for the first time needs a restart, because its hooks must be in before Lossless Scaling asks
  for displays), it starts earlier, its options are two combo boxes and two checkboxes instead of a panel, and the addon list no longer has two entries that are
  really settings. An old `LSP-ReShade` or `LSP-Windowed` addon folder is ignored, so the two versions can never both run; the deploy script moves such folders
  aside. MinHook (pinned, v1.3.4) is now a dependency of the manager. Offline test `lsproxy_featurestest` (run twice: Windowed on and off at start-up) replaces the
  two old addon tests. The release now ships one addon (Neural Rendering) instead of three.
- **Manager: addon handling, settings file, host interface, toggle switch, toast, addon card and logs tab rewritten** as smaller modules with the same behaviour
  (tried in Lossless Scaling before release). New offline test `lsproxy_coretest` covers scanning, manifests, loading, starting, switching,
  removing, installing, the security levels, a faulting addon, the settings file, the host interface, the safety checks, the dependency order and the event bus (126 checks); the window pictures are pixel-identical
  before and after (`tools/compare_ui_renders.py`). Small fixes on the way: a wrong-typed field in `addon.json` is skipped instead of dropping the rest of the
  manifest, and loading a second settings file no longer inherits what the first one last saved. The addon safety checks (SHA-256 against `trusted_addons.json`), the dependency ordering and the event bus were rewritten too, with fixes: the addon list
  no longer reverses its order every time it is sorted (installing or removing an addon used to flip it), a trusted-hash list that is loaded again replaces the old
  one instead of adding to it, a hash written in capitals matches, and a subscriber that faults is now logged. The share of the manager's code that is still the
  original project's went from 42.8% to 31.3% (`tools/measure_original_share.py`). Counting the ReShade and Windowed features, which moved into the manager
  and began as the original project's addons, it is 32.7%.
- **DLSS 5 Neural Rendering: Ghost guard.** New slider that fades the model's change where Lossless Scaling's motion data is unreliable and as the change ages, to remove the faint
  copy of the previous frame that could trail moving things. Default 0.5; 0 restores the old behaviour. See the addon's changelog.

## 0.1.0 (first release)

The first release of Echo Addon Manager as its own project, with a fresh history. It started from FrankBarretta's
[LosslessProxy](https://github.com/FrankBarretta/LosslessProxy) (see [NOTICE.md](NOTICE.md)).

### Manager
- Loads in place of Lossless Scaling's `Lossless.dll`, forwards to the original, and loads addons from `addons\`.
- Window with tabs Addons, Performance, Settings, Logs and About, in a dark neutral and green look with vector icons, tooltips on every control,
  and an interface size setting (75% to 200%). Hides to the notification area; opens again from its icon or a hotkey (Ctrl+Shift+F12 by default).
- **Install addon** from a folder, zip or DLL (button or drag and drop; new addons arrive switched off) and **Remove** with confirmation (moved to
  `addons\.removed`, never erased). Addon search (Ctrl+F). Each addon's settings, overview and config file in a detail pane.
- **Performance tab**: game frame rate and frame time, addon cost, GPU load, power, clocks, temperature and memory (NVML, read only while the tab
  is open), 20-second graphs and a plain-words summary.
- **Live status and metrics**: `SetStatus` and `PublishMetric` on the addon interface; a status line on each card and in the status bar.
- **Backup and restore** of all settings; **diagnostics zip** with logs, settings and a summary (nothing uploaded).
- Settings that work and save at once: security level, log detail, auto-load, open at start, hotkey.
- Atomic settings writes, a corrupt settings file is kept rather than replaced, a bounded log with timestamps, crash backtraces for the manager
  and any addon, a faulting addon is isolated, and Dear ImGui is pinned so addons and the manager share one layout.
- Addon API **1.0.0**, separate from the release number (`sdk/include/lsproxy/version.h`).

### Addons
- **DLSS 5 Neural Rendering** (andreiday, extended): read-only tap of Lossless Scaling's frames, a free-running model, present-time compose.
  Saved looks in a bar at the top of the panel, per-program looks, rectangles that keep the HUD untouched, shadows and highlights, sharpen,
  saturation and vibrance, film grain, temporal smoothing, compare and hotkeys, a live status line. Its own history is in
  [its changelog](addons/DLSS5NR01/CHANGELOG.md).
- **ReShade Input Passthrough**: reworked as a plain switch with a tooltip; window handling restored cleanly and pinned when it cannot be undone. Offline lifecycle test.
- **Windowed Mode**: reworked as a plain switch, panel fixed; adds the virtual display only while enabled. Offline test.

### Tools and tests
- `tools\build_all.ps1`, `deploy.ps1` (backs up, refuses while Lossless Scaling or your game runs), `run_addon_tests.ps1`, `run_hosttest_matrix.py`
  (ten Neural Rendering scenarios), `ui_preview.ps1` (offscreen renders of every tab), `package.ps1` (the release zip).
