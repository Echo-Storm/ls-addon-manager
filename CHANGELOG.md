# Changelog

## 0.9.26 (2026-10-01)

**Fixes from the issues (#11, #12, #13), a card that says when the recorder is the problem, and a test option for a full card that is safe to try.** Nothing here changes a picture unless an option is turned on, except the ultrawide fix (issue #13) and the Auto quality hold, which only act in the cases they were made for.

- **A fault in the upscaler's pass says where it was.** The log line for an exception in the DLSS scaler ("DISABLED: exception 0x...") now also gives the module and offset (and what an access violation touched) and the step the
  pass was at, so a report of one can be found in the code; the offline test host prints the same for a crash on any thread.
- **"Record what is shown" keeps the middle 1920x1080 of each presented frame, and the card says when the recorder is the problem.** The owner's first recordings of what is shown (4K, HDR, frame generation x2) kept 84 of 247 frames
  and compressed to more than their raw size: a 4K HDR frame is 66 MB and every presented frame is copied. The middle of the frame at full size (an even offset) is kept instead, a quarter of the data, which a card in use can
  copy; a flicker shows in the middle as well as anywhere. And "What stands out" now says "Recording is on ... it is the first thing to rule out when frames are uneven" (a problem when they are, a note when they are not): the same
  session's log showed a full card (the model waiting 19.7 ms, a picture 1,016 ms late) exactly while the recorder was saving.
- **`nr_lsrec flicker <recording>`**: for a recording (of what is shown, with "Record what is shown", or of anything) how much the picture changes from one frame to the next, by kind of pair, the biggest steps with the time between
  frames, and whether the change pulses at a period: the objective side of "it flickers".
- **The panel says whether your model file is a build seen working.** People have different `nvngx_dlssnr.dll` files, and a name, a version and a size do not tell two builds apart (issue #12: an RTX 4080 refused a file that looked like the tested
  one). The Requirements check now makes the file's SHA-256 (the first 16 hex digits) and says "a build seen working (file 4b8d...)" when it is on the list, and, for a file with the tested version and size that is not, "not the same file":
  the first thing to check if the compatibility test fails. The list (`kKnownModels`, and docs/model-compatibility.md) grows with the reports people send; a build seen failing is not on it.
- **The compatibility report says more when the model refuses (issue #12: an RTX 4080 got "NOT_SUPPORTED", which no Ada card should).** The report a person pastes into an issue now also carries: every graphics adapter the system has and
  which one was tested; the first 16 hex digits of the model file's SHA-256 (a name and a size do not say whether two people have the same build); what NVIDIA's NGX said while the test ran (the last 40 lines, folders left out); and, when
  the model refuses, what it does for the same feature at other sizes (1920x1080, 960x540, 640x360, 2560x1440 and 1280x720 again), which shows whether the refusal depends on the size.
- **Neural Rendering on an ultrawide screen with a game that is not (issue #13).** A 2560x1440 game on a 5120x1440 screen is drawn by Lossless Scaling with black bars each side; the model's change belongs to the picture, but the compose
  stretched it over the whole screen, which put the picture's change on the bars and misplaced it on the picture ("extreme artifacting"). The compose now works out where the frame is drawn (its shape fitted into the screen, centred) and
  leaves the bars as they are, after checking that they are there (a one-pixel strip through the middle of where each bar would be, copied now and then, must be dark: in Lossless Scaling's stretch mode there are none and nothing changes); the log says so ("the 2560x1440 frame is drawn into 5120x1440 with bars", "the bars are there"). "The picture fills the whole screen" (Neural Rendering, Advanced) is for Lossless Scaling's stretch mode, where there are no bars.
  New checks in the suite (`nr_composebench viewport= barcheck=1`, and the host scenario `ultrawide_bars`).
- **A runtime that crashed Lossless Scaling is not tried again (issue #11).** The FSR Upscaler's FSR 4.1.1b INT8 runtime (chosen in the Runtimes list) closed Lossless Scaling on a Radeon RX 6600 XT as soon as it scaled a window. A runtime that is not
  the shipped one is now marked "on trial" in a file (`runtime-trial.txt` in the addon's folder) from its start until it has upscaled ten seconds of frames or Lossless Scaling closes normally; found at the next start, the shipped runtime runs
  instead and the log says why. To try it again, choose the shipped one in the Runtimes list and then the other again.
- **Auto quality no longer cycles between every frame and every 2nd or 3rd.** The owner's trace showed it: the model skips frames, the game recovers, the governor calls that calm and goes back to every frame, the game slows again, and
  so on every 40 to 60 s, each change a visible change in how the picture is made. When going back did not hold (the game was slow again within 90 s) the hold at every 2nd or 3rd now doubles each time (30 s, 1 min, 2 min, up to 10
  min), and starts afresh after five calm minutes at every frame.
## 0.9.25 (2026-10-01)

**Safeguards for a full graphics card, and a recorder that records what you see.** From a player's report (a 4090 at 40 to 50 fps: even with Lossless Scaling's frame generation alone, uneven with an addon, a one-frame warp of the whole picture with
Neural Rendering) and from the owner's flicker: two guesses at the cause are guarded against, the real output can now be recorded for looking into it, and the addons say how much video memory they hold. Nothing changes a picture unless the card is held up.

- **Two safeguards against the catch-up hitches a player described with Neural Rendering or the DLSS Upscaler on a full card** (a 4090 at 40 to 50 fps: Lossless Scaling's frame generation alone was even; with DLSS "uneven, catching up", with Neural
  Rendering added "fine for about 120 frames, then one or two frames of 100 ms with the whole picture warped"; G-Sync, so not the display). (1) The upscalers wait on the GPU for their picture rather than show one twice, which holds Lossless
  Scaling's queue up with the upscaler's: when the game holds the card, one very late picture is a hitch of that length and the frames behind it catch up. A picture that takes over 50 ms now switches the GPU wait off for 10 s (a picture
  is repeated instead, a hitch of one frame), and says so in the log; the periodic line counts the slow pictures and the slowest. (2) Neural Rendering moved a model result that arrived many frames late along the motion by all of them, which
  warps the picture; a result more than three frames old (plus one for each frame auto quality skips) is now left off that picture, and the log says so. Both are guesses from the description, to be checked against their logs and traces.
- **The addons log how much video memory they hold.** The owner saw Lossless Scaling hold over a gigabyte more than it had been (15.2 to 14.1 GB when it closed). Each addon now logs the process's video memory (DXGI's own figure)
  before its engine starts, after it, and with the periodic lines ("video memory: this process holds N MB on the card ... N MB since the engine started"). `nr_sreval` and `nr_nreval` print what their engine adds. Measured offline at 2562x1442
  in HDR: the upscaler (DLSS, with its textures) adds 281 to 372 MB (sharpening and steady sharpening on, 1.5x or 1:1), Neural Rendering 298 to 412 MB (model at 25 % to 50 %); at 4K the frame-size parts are 2.25 times that, and the
  upscaler's shared textures with Lossless Scaling (two frames, three pictures, 66 MB each at 4K in HDR) come on top: with both addons on, over a gigabyte is what they hold.
- **"Record what is shown" now works for the upscalers with Lossless Scaling's frame generation.** The setting said "(with frame generation of our own)" and did nothing else: the recordings stayed the frame going to the
  upscaler, so a flicker seen on screen could not be looked into (the recordings of 2026-10-01 were the inputs, and showed no flicker the addons add). With it on, the upscaler addons record the output swap chain's back
  buffer at Present, as it goes to the screen (after the upscaler, the generated frames included, tagged), in place of the upscaler's input. The recording can be scored with `nr_sreval`'s no-reference flicker, or toggled
  between the enhanced and the plain picture (F7) in the same scene to see what the chain adds.
## 0.9.24 (2026-10-01)

**Pacing tools, a card that backs off, and a test option for a full graphics card.** The frame trace and the "What stands out" card are for the next "bumpy pacing" report; Neural Rendering now spares a full card; the upscalers'
motion is right at frame-generation ratios that are not whole numbers; a settings file with nonsense in it cannot break the addons. Nothing here changes a picture unless an option is turned on.

- **Fixed before release: the lighter upscaling went on every picture, and menus stayed on screen** (found in the owner's first live run with the option, 2026-10-01). In HDR the frames and the flow fields are both RGBA16F, so a
  plain copy of a frame passed for a generated-frame pass, and every presented picture counted as generated: the real frames got the lighter run too, the upscaler never ran again, and the lean blended toward a
  picture that was never made again (offline, with every frame asked for the lighter run: 15 to 17 dB against 24, and a menu that left took ten frames to go; `tools/make_menu_clip.py` makes the clip, `nr_sreval realonly=3`
  asks for it). Three guards now: a flow field counts only when it is smaller than the frame; a test that is true for more than 16 pictures in a row is taken as not telling them apart (no lighter run, and the log names
  the pass that set it); and the engine never does more than 8 lighter runs in a row, whatever it is asked. Without the option on, nothing changed. The owner's next log showed the second guard working (16 pictures
  got the lighter run, then it stopped, and the pass it named was NIS's own: the tap took the 4K output for the frame's size); the frame size now comes from the NIS pass's input, and the log says once, after about 900
  pictures, which picture after a real frame is the captured frame itself and how many generated-frame passes were seen, so that the real frames can be told for sure. Until then the option may do nothing on a setup where
  a generated frame is not a compute pass of the kind the test knows; that is safe.

- **"Lighter upscaling of generated frames (test)"** (the three upscalers, off by default). With frame generation the upscaler runs on every presented frame, and in the owner's HDR 1.5x log that was 3.3 ms of GPU
  for each (6.6 ms for each frame the game draws, about two thirds of everything the addons add at the spot where the game fell from 60 to 40 fps). With the option on, the frames the game draws get the full upscaler
  and the frames Lossless Scaling makes between them get the last upscaled picture, moved along the motion by their share of the step, checked against the new frame at the scale of a few pixels (where they disagree, the new
  frame stretched plainly), and sharpened as usual: under 1 ms instead of 3. A generated frame is recognised by the generated-frame pass that runs before it. Measured offline on four recordings (docs/frame-generation-research.md):
  detail and flicker about equal, 0.4 to 0.8 dB lower in fast motion, a little lower on the steadiness score. Not yet seen by eye: flip it with the camera turning and in a calm dark scene. The frame trace marks these frames,
  and the log's "frames upscaled" line counts them.
- **The upscalers' "NIS stays" line is no longer alarming while they work.** Lossless Scaling draws a second kind of pass into part of the screen, which the upscaler leaves to NIS; the log said "in a way the upscaler cannot follow
  yet ... please report it" about it every 30 s, in logs where 96 % of the frames were being upscaled. Once frames have been upscaled the line says what it is and that it is normal.
- **The upscalers scale the motion by time, not by a count of presents.** With frame generation the upscaler is told how far each presented picture is along from the one before. It took that as 1 over
  the number of presents between the last two real frames, which is right for a whole multiplier (x2, x3) but not for Lossless Scaling's adaptive mode or a screen whose refresh rate is not a multiple of
  the game's (120 Hz at 50 fps is 2.4 presents to a frame, so 2 and 3 by turns): the step then alternated between 1/2 and 1/3, up to 20 % wrong on every frame, and the motion the upscaler used shook.
  It is now the smoothed time between presents over the smoothed time between real frames (the log's "presented per real frame" line says "each 0.42 of a real step"). Unchanged for x2 and x3.
  A reported symptom this may explain (bad pacing at 40 to 59 fps on a 120 Hz screen, clean at 25 to 39); not yet seen live.
- **A settings file with nonsense in it cannot put the addons out of range.** A new test feeds every setting a list of bad texts (nan, inf, 1e308, negatives, words, empty). It found that "nan" in
  any number got through the limits (a comparison with nan is never true), that a few settings had no limit at all (the model's intensity, fine detail, local contrast, the compose blend, the
  per-pixel limit, the protect-bright start, the flow unit, the slow-model watchdog, the tap mode) and that a huge number could overflow the conversion to a whole number. Numbers that are not
  finite now read as the setting's default (or, in a look, leave the setting as it was), and every setting is limited to a little wider than its slider.
- **A reset of the history was lost when its frame was left out (Neural Rendering).** "Reset history", a change of the model's size and a rebuild each ask for a reset with the next frame given to the model;
  if that frame was left out (the model busy, or its turn off under auto quality's every-Nth setting) the request was spent and the history never reset. It now waits for a frame that runs
  (Neural Rendering; the upscalers already kept theirs).
- **A "What stands out" card in each addon's panel.** From the numbers the addon already keeps (how even the game's frames were over the last few seconds, what the model waited for, what auto quality
  is doing, what the upscaler costs) it says in plain words what is wrong and what to try: "the game's frame times are uneven... the graphics card is full", "the model waits 9 ms for the card",
  "auto quality is running the model on every 2nd frame", "you are looking at the plain picture (the before / after toggle)". It stays empty when nothing stands out. New test `nr_diagtest`.
- **A frame trace for pacing problems.** The logs keep averages, which is why "bumpy pacing" could not be looked into. The Neural Rendering and upscaler addons now keep a timeline of the last
  131 000 events (a real frame arriving, every present, each model run with its GPU start delay, each upscaler pass, auto quality's changes, hotkeys), at a cost of a timestamp and four integers
  a frame, and write it to `logs\frame-trace-<addon>.csv` at shutdown and whenever a recording is saved (Ctrl+Shift+F1). `tools/analyze_frame_trace.py` reads it: the interval of the
  presented and the real frames (p50, p95, p99, low 1 %), the longest gaps with what else happened around them, whether the model's runs line up with the gaps, and the upscaler's and auto
  quality's numbers. Sending that file with the logs is enough to see where a stutter came from. New test `nr_tracetest` (threads, wrap-around, export).
- **Neural Rendering's compose pass is about 10 % cheaper in HDR.** It is one pass per presented frame (two per real frame with frame generation) and cost 0.94 ms of GPU at 4K in HDR with
  sharpening (0.98 in a live log). The sharpening converted each neighbour's SDR view again for every pixel that reads it, and the way back into the frame's encoding made two round trips
  through the view that are the identity: now each pixel's view is made once per 8x8 tile and the round trips are replaced by direct light conversions. 0.94 -> 0.83 ms (0.59 -> 0.56 without
  sharpening); the result is the same up to fp16 rounding (one half-float step at most; a mean difference of 0.00003). SDR is unchanged. Measured with the new `nr_composebench`.
- **Neural Rendering backs off further when the game's frames stay slow.** From the owner's log: at the lowest model resolution (the floor, 0.25) the model still took 2 to 6 ms and waited 6 to 22 ms for the
  card, and while it ran the game's frame time spiked (p95 17.6 ms with Neural Rendering off, 29 to 31 ms with it on, at the same spot at 60 fps): judder, and what a player described as bumpy pacing. Auto
  quality now also runs the model on every 2nd (then 3rd) real frame when the game's frames are still well over the best they have lately managed even at the floor (the presents keep warping the last
  result, as they already do while the model is busy), and goes back to every frame after the game has been steady for 10 s (not before 30 s after the last increase). The log says when ("auto: the model now
  runs on every 2nd frame"). Only with auto quality on. Not yet seen live.

## 0.9.23 (2026-09-30)

**One more HDR fix, straight after 0.9.22's.** In HDR the Catmull-Rom lean (the picture the upscalers lean on in fast motion and, with Sharpness at rest, at rest) could ring around a small bright
highlight: its negative lobes overshot it by about 25 % in light, which with the sharpening on top read as sparkle. It is now held within its four nearest texels' range, as EASU always was.

- **HDR: the Catmull-Rom lean can no longer ring around a bright glint.** Its negative lobes overshoot a small bright highlight by about 25 % in light (peak 2.39 against an input peak of 1.90 on the
  test clip); in HDR the result is now held within its four nearest texels' range, as EASU's always was (peak 1.90; with all the passes on, 2.80 -> 2.23). SDR frames are untouched.

## 0.9.22 (2026-09-30)

**A major bug fix for HDR, and a steadier Neural Rendering.** With HDR on, sharpening could turn a highlight into a white speck up to 10,000 nits bright; that is fixed in all three upscalers. The same
release lowers the flicker of Neural Rendering (auto quality no longer ramps, the smoothing trusts its history where the picture is unchanged), sharpens less where the picture moves fast, and the
FSR Upscaler now sharpens the way the DLSS and XeSS Upscalers do. The HDR fix comes first; the rest is below it.

- **HDR: sharpening could turn a highlight into a 10,000-nit speck.** The HDR view's roll-off is steep at its top (a view of exactly 1.0 is 125 times the SDR white), and the
  sharpening passes and the picture controls (brightness, contrast, gamma, shadows, highlights, saturation) work in that view and put only their change back into the light: a bright pixel
  that a change nudged to 1.0 came back as a white speck. Found by running the upscaler on an HDR recording in fp16 (new `nr_sreval hdr=1`): the peak light of the output was 125.75 against
  1.20 in the input. Now a change made in the view goes back onto the light within 1.5x of it (plus 0.5 of the SDR white; dark and mid tones are untouched), and the sharpening's result stays
  within 15 % of the range of its neighbours' light (no ringing; not applied to SDR). On the same footage the peak is 1.44; the mean light is kept (out / in 1.003) and the scores are the
  same. Covers the plain and the steady sharpening passes (DLSS, FSR, XeSS) and the picture controls of the upscalers. A test for it (a made-up HDR clip with bright glints) is in the
  Neural Rendering suite of `tools/run_addon_tests.ps1`.
- **"Sharpen less in fast motion" (0.5 by default) in the upscalers' panel.** The eye cannot resolve the extra detail sharpening adds where the picture moves fast, but it does see
  the shimmer sharpening amplifies. The sharpening is lowered smoothly from 1 to 8 pixels of motion a frame, by this much at the fastest; at rest and in slow motion it is untouched.
  On a recording walking over cracked ground at 1:1 (DLSS model E, sharpening 0.45): flicker added over the game's own 113 % -> 107 % at 0.5 (102 % at 1.0), detail kept 137 % -> 127 % (117 %):
  more detail for the same flicker than simply sharpening less (which would keep about 121 % of it for 107 %). Upscaling 1.5x, scored against the true picture, 0.5 is better than off
  (Silent Hill f 43.82 dB against 43.01, steadier 41.21 against 40.43), at the same pass cost. Works for DLSS, FSR and XeSS.
- **Neural Rendering's temporal smoothing trusts its history more where the game's picture is unchanged, and starts at 0.7 (was 0.4).** Where a pixel's brightness matches what was kept
  with its history (the same surface), any change in the model's output is the model's own noise, not new content, so the history weight there goes up to 1 - (1 - setting) / 4 (0.95 at 0.8,
  at most 0.97) while the setting still rules where the frame changed. From the design notes of the Magpie fork (docs/magpie-fork-study.md), measured with `nr_nreval` on your own clips,
  the model's change where the game's frame is still, in levels of 255 (Durotar, working scale 0.3: setting 0.4 -> 0.72 before, 0.43 now; 0.8 -> 0.40 before, 0.29 now; character screen: 0.4 ->
  0.42 before, 0.20 now; 0.8 -> 0.21 before, 0.11 now). Existing settings keep their value (the slider is on the panel's Neural Rendering page).
- **Neural Rendering's auto quality no longer ramps.** In a live log it started at 1.00 and stepped down nine times in 100 s (1.00, 0.90, 0.80 ... 0.25), and each step rebuilds the model with no
  history: the picture changed look every few seconds, and 2 to 6 % of presented frames had no change applied while it rebuilt (up to 82 % while a 6 GB recording was being saved). Now it
  (1) goes straight to the resolution where the model's time is expected to fit, from its measured time and area (fixed cost and per-area cost from two measurements), in two changes
  from 1.0 instead of nine; (2) starts from the resolution it last held for 30 s within budget (`autoScaleLast`, kept once a minute at most); (3) tightens the budget in proportion when
  the game's own frame time is well over the best it has lately managed (the card is out of room), and does not go back up for a minute; the log says when it did.
- **With frame generation, every other presented frame keeps the motion estimate of the one before** ("Share motion between generated frames", on by default; it does nothing
  without frame generation). Two frames are shown for each one the game draws and the motion from one to the next is about the same for both steps. The estimate's time
  fell from 0.39 to 0.23 ms a frame on average at 4K with the same scores on the recordings (used here as a harsher test: their steps are whole game frames, not halves).
- **The passes after the upscaler move fewer bytes.** In HDR they are limited by memory traffic (about 0.97 ms at 4K in one log, six 66 MB pictures read or written): Steady
  sharpening's running average is now kept in 10 bits a channel (4 bytes instead of 8; it holds the SDR view, 0 to 1) and is not read at all where the pixel moves too fast to
  trust it. The same quality on the recordings; the gain shows in HDR (`nr_sreval bench=N` times the passes back to back).
- **Steady sharpening is on by default (0.6)** and no longer says "test". The FSR Upscaler now sharpens with the same pass as DLSS and XeSS instead of AMD's RCAS, so it gets
  Steady sharpening too and the Sharpening slider means the same on all three: at 0.5, RCAS left 73 to 83 % of the game's detail while the pass leaves 101 to 112 % (as it does for
  DLSS and XeSS), and FSR scored about 0.5 dB closer to the true picture upscaling 1.5x. Saved settings keep their value.
- **Steady sharpening trusts its running average less where the picture moves fast** and is now a single pass (cheaper than the two passes and a copy it was in
  0.9.21). Upscaling 1.5x it now scores within 0.1 dB of no steady sharpening (0.9.21 lost up to 0.96 dB on a moving clip); at 1:1 it keeps its gain on a nearly still scene
  (docs/frame-generation-research.md).

## 0.9.21 (2026-09-30)

Sharper upscalers at rest, with the shimmer kept down.

- **Steady sharpening (test), a new slider in the upscalers' panel** (off by default). Sharpening amplifies the game's own shimmer as well as its detail. With this on,
  the sharpening is worked out from a running average of the picture (followed along the measured motion and kept within this frame's neighbourhood, so a wrong
  motion cannot ghost) and only that sharpening is added to the current frame: the detail stays, the shimmer is not amplified. `nr_sreval` on World of Warcraft
  clips at 1:1: about 10 points less flicker for the same detail on a nearly still scene and 1 to 2 in motion, never worse than plain sharpening
  (docs/frame-generation-research.md). Upscaling 1.5x it is neutral to slightly worse in motion, so it stays off by default. Tried live in HDR by the project owner: sharper at rest, no
  HDR problems. Costs two more passes and one copy. Try Sharpening 0.5 to 0.7 with Steady sharpening 0.6 to 0.9.
- **A new default: Sharpness at rest 0.5** (was off). Upscaling 1.5x and scored against the game's full-size frames (three clips), it is closer to them and steadier at every
  step: 1.3 dB closer and 1.4 dB steadier at 0.6 than at 0. It applies to new installs and to any setting never saved; a saved value stays.
- The same measurements found that "Crisp edges when moving (EASU)" scores lower (about 0.15 dB on WoW, 1.75 dB on Silent Hill f) though it looks crisper, and that "Motion by
  shape (test)" changes nothing measurable (under 0.03 dB); both stay off by default.

## 0.9.20 (2026-09-29)

A bug sweep: every test suite run in full (the manager, the installer, the window, and all 48 Neural Rendering scenarios), plus a review of what changed since 0.9.12.

- **The DLSS Upscaler's log no longer fills with one line.** "...cannot follow yet, so NIS stays" was written every frame in a setup where a second kind
  of pass flips it on and off (6,681 identical lines in eight minutes in one user's log, about 13 MB an hour); it is now logged when it changes and at most once
  in 30 seconds.
- **The manager's GPU reading** (the header and Performance tab) can no longer be started, or read, from two threads at once (the sampler and the
  diagnostics button), which the Windows-counters fallback of 0.9.16 made possible.
- **The shared recorder** hands its setting to the other addons only when it actually changed: an addon holding an old value (from before the upgrade)
  no longer switches another addon's recorder off by saving an unrelated slider.
- **CI is green again** (it had failed since 0.9.16 on a test that assumed an NVIDIA card; fixed in fe42024).
- **Tests:** the checks that still expected DLSS to beat "no motion" on a sliding picture (it cannot without the game's camera jitter) and the log wording of the
  compatibility test were brought up to date. Nothing in the shipped files changed for those.

## 0.9.19 (2026-09-29)

For the upscalers: sharpness when the picture stands still (issue #8), and a test of crisper edges when it moves.

- **"Sharpness at rest"** (new slider under "Steady in fast motion", off by default). Reported: sharper while moving, softer when standing still.
  Both are the same thing seen from two sides. Moving, the upscaled picture leans on a resample of the game's frame (sharper); at rest it is the
  upscaler's own picture, and without the camera jitter a game gives its upscaler it has no extra detail to find, only its smoothing to give.
  Measured on recordings with the frame held still, DLSS scores 32.5 dB against 33.6 for a plain resample, with 73 % of the game's detail against 80 %
  (World of Warcraft: 58 % against 65 %). The slider keeps that share of the resample in the picture even where nothing moves (0.25: 32.9 dB,
  0.5: 33.2, 1: 33.6), trading some of the upscaler's anti-aliasing at rest for sharpness; sharpening adds on top. Try 0.3 to 0.5.
- **"Crisp edges when moving (test)"** (off by default): the resample the lean uses in motion can be AMD's FSR 1 edge-adaptive filter (EASU, ported to HLSL,
  in NOTICE.md) instead of Catmull-Rom: cleaner, crisper thin and diagonal edges. Measured shimmer and detail are the same; the difference is in how
  edges look, so it is there to compare by eye. On the host test's sliding picture it is closer for FSR (11.7 against 14.2 levels off).
- **Tools:** `nr_sreval` takes `restmix=`, `still=1`, `lean=easu`, `sample=point` (aliased input, as a game without anti-aliasing renders at a lower size),
  `stability=`, `fastp=`, `saveframe=`, and prints the plain stretch beside its no-reference scores. Findings in `docs/frame-generation-research.md`:
  the three upscalers side by side, how far the lean should go, and why a temporal upscaler cannot add detail without jitter.

## 0.9.18 (2026-09-29)

The recorder is one setting for all the addons.

- **Recording works whichever addon's panel you switch it on in.** Each addon had a recorder of its own, and switching it on in one that was not
  the one working on the frames (the FSR Upscaler's, with the DLSS Upscaler running) recorded nothing: the save key answered "nothing recorded
  yet". Now the Recording section is one setting (on or off, seconds, memory, folder), kept in every addon's settings and followed by each running
  addon within half a second. The save key says so when the recorder is off, and where to switch it on.
- **Switching on "Motion by shape (test)" cannot stall Lossless Scaling any more** when the engine stops while its extra shader (about 7
  seconds to compile) is still being made: that compile owns its references and lets go of them when done.
- **Tools and tests:** `nr_nreval` runs Neural Rendering's model on a recording and scores its flicker (still areas, along the motion with a heat
  map, the live path with the model's own motion; findings in `docs/frame-generation-research.md`); `nr_sreval` scores shimmer and detail with no
  reference picture (for 1:1) and takes `sharpen=`: on recorded World of Warcraft at 1:1, sharpening 0.7 gives 2.3 times the game's own detail and
  37 to 55 % more frame-to-frame shimmer, 0.3 gives 1.5 times with 8 to 17 % more; about 0.1 to 0.15 is neutral. The host test that expected DLSS
  to follow a sliding picture better than with no motion now checks the motion path does no harm (DLSS cannot benefit without the game's camera
  jitter; the upscaler leans on the new frame by design).

## 0.9.17 (2026-09-29)

The upscalers start in about 1 second again.

- **The upscalers no longer take ~9.5 seconds to start** (0.9.14 to 0.9.16). Each start compiles the motion estimate's shaders, and the search
  shader had grown to 6.4 seconds of compile time, from the experimental "Motion by shape" matching, which is off by default but was compiled
  for everybody. Until the engine was ready, Lossless Scaling's own NIS showed instead. The plain search compiles in half a second now (its
  output is bit for bit what it was), and the "shape" variant is made on a thread of its own, only when "Motion by shape (test)" is
  switched on, and takes over once it is ready. (The host test `scaler_bgra`, which had been failing for this reason, passes again.)

## 0.9.16 (2026-09-29)

Fixes for four issues reported on GitHub.

- **The DLSS Upscaler starts with Neural Rendering on** (issue #7: "FeatureNotFound (nvngx_dlss.dll missing?)" although the file was there,
  on a 4090). NVIDIA's NGX core is one per process and keeps the search folders of the first addon to start it; Neural Rendering's
  did not include the DLSS runtime's folder, so with both on and Neural Rendering first, the DLSS Upscaler could not find its runtime. Found by
  reproducing it offline (the same error, with the runtime folder right there); every addon of ours now passes the same folders, in any
  order. The compatibility test uses the same list.
- **The Performance tab and the header work on AMD and Intel cards** (issue #4, RX 9060 XT): GPU load and memory in use now come from
  Windows' own counters (the ones the Task Manager shows) where NVIDIA's NVML is missing; power, clocks and temperature are not reported
  there and are left out. The tab also listens to the upscalers now (frame times, their cost), not only to Neural Rendering: it was empty
  with an upscaler alone, on any card.
- **Two NVIDIA cards** (issue #6): the compatibility test tests the card Lossless Scaling runs on (the model runs there), not always
  the first NVIDIA card DXGI lists, and the log says which card it tested. Which card that is depends on Lossless Scaling's Preferred GPU.
- **Deploying from source** (issue #5, `tools/deploy.ps1`): Lossless Scaling's own `Lossless.dll` is renamed `Lossless_original.dll`
  before ours takes its name (it was only backed up, and Lossless Scaling would not start). New: [docs/antivirus.md](docs/antivirus.md), what to do when
  an antivirus flags the download as a trojan (a cloud verdict on a new, unsigned file; Defender's own scan of the release finds nothing).
- **Tools:** `nr_sreval` can start NGX Neural-Rendering-style first (`ngxfirst=1|2`), `eam_gpuprobe` compares NVML with Windows' counters.

## 0.9.15 (2026-09-28)

A fix for Neural Rendering in HDR.

- **No more neon orange and cyan with Neural Rendering in HDR** (issue #3, and likely #1's "too bright"). Since 0.9.9 the curve that
  takes an HDR frame into Neural Rendering's view and back rolls highlights off logarithmically (so the upscalers keep them); its
  inverse is steep near the top, so where the model brightened a colour in one channel, that channel alone came back at over a thousand
  nits: fluorescent orange or cyan in bright areas, reflections and highlights. The model's change now never lifts a channel above 1.35
  times the SDR white, unless the game's own pixel was brighter there already (close to how 0.9.8 behaved). Windows Auto HDR, scRGB and
  HDR10 alike. Tested: the HDR host tests (scRGB and HDR10).
- **Neural Rendering's temporal smoothing** defaults to 0.4 (0.6 in 0.9.14 was too much; a saved value is kept).
- **The save-recording key** says when the recorder is off in the addon that answered, and where to switch it on: each addon has its
  own recorder (it said "nothing recorded yet").
- **Tools:** `tools/setup_obs_for_tests.ps1` sets OBS Studio up for recording what the addons show (the whole display, NVENC AV1 near
  lossless, a replay buffer saved with the addons' own save key).

## 0.9.14 (2026-09-28)

The upscalers in motion: a very big improvement. Less trailing and smearing while you walk and turn, no flicker at 1:1, and all three
upscalers now beat a plain stretch in a fast turn, for 3-4 ms a frame at 4K.

- **The upscalers lean on the new frame from much less motion.** Lossless Scaling gives an upscaler none of the sub-pixel camera jitter
  a game gives it, so in motion its memory of earlier frames adds no detail, only trailing. Measured offline, it trusted our motion
  vectors and kept that memory: with them it did worse than with none. So the upscaled picture now leans on the new frame from about
  1 px of motion a frame when upscaling (was about 13 px) and from 0.1 % of the width at 1:1 (was 0.5 %). A recorded Silent Hill f turn,
  upscaled 1.5x (upscaled / coarse, where trailing shows / steadiness, in dB; a plain stretch 32.68 / 43.94 / 29.78):
  - DLSS 32.82 / 43.18 / 30.11 -> **34.02 / 47.17 / 31.13**
  - FSR 33.44 / 45.47 / 30.59 -> **34.10 / 47.84 / 31.20**
  - XeSS 33.61 / 46.42 / 30.72 -> **34.07 / 47.82 / 31.18**

  World of Warcraft walking, 1:1 with DLSS: coarse 45.70 -> 47.82. The owner's verdict, live in World of Warcraft: all three "worked and
  look better". New "Lean from" slider under "Steady in fast motion" to set it by eye (automatic by default).
- **No more flicker while moving at 1:1 with DLSS.** Model L pulses on a four-frame cycle when DLSS runs at the screen's size (found
  offline on recordings: about 9 times a second at 36 fps); E does not. The DLSS model list has a new default, **Auto**: E at 1:1 (DLAA),
  L when upscaling. A model chosen before is kept (pick Auto once to switch).
- **Cost, live at 4K 1:1 in World of Warcraft** (RTX 4070 Ti SUPER, GPU time a frame, the motion estimate's share in brackets): DLSS
  4.20 ms (1.64), FSR 3.3-3.7 ms (1.1-1.5), XeSS 2.7-2.8 ms (0.7).
- **Before / after hotkey:** the upscaler addons now show the corner square too (green the upscaled picture, red the original, amber
  the split), as Neural Rendering does.
- **Neural Rendering:** Temporal smoothing works against the model's flicker in motion now: it keeps its history where the frame itself
  still matches along the motion, and holds back only where something new came into view. On by default (0.6) for new settings; a saved
  "off" is kept, so move the slider to try it. Still the weakest part in motion: more to come.
- **Motion by shape (test):** a checkbox in the upscalers' panel (off by default) matching the motion by shape and edges rather than
  brightness, for scenes where the light changes.
- **Tools:** `nr_sreval` scores steadiness (shimmer from frame to frame) and prints each frame's change from the one before; the motion
  estimate's block cost can be varied (`meanweight`, `gradweight`). Findings in `docs/frame-generation-research.md` and
  `docs/lsfg-vk-study.md` (how Lossless Scaling's frame generation is built, studied from lsfg-vk).
- **Known:** Neural Rendering still flickers in motion; open items in `handoff/BACKLOG.md`. Frame generation of our own is not in this
  release.

## 0.9.13 (2026-09-27)

The upscalers steady in fast camera turns.

- **Steady in fast motion (all three upscalers).** Lossless Scaling's frames carry none of the sub-pixel camera jitter a game gives its
  upscaler, so in a fast turn the upscaler's history adds little and trails: leaves and edges with soft doubled outlines. Now, where the
  picture moves fast (from 0.5 % of its width a frame) or its motion cannot be followed, a pass after the upscaler blends its picture toward
  this frame upscaled plainly. DLSS needed it most: its DLSS 4 models ignore the mask a game would give it for this. Measured offline on a
  recorded Silent Hill f turn, shrunk 1.5x and upscaled back (at a quarter of the size, where trailing shows; a plain stretch 43.3 dB):
  DLSS 36.8 -> 44.2, FSR 41.7 -> 45.6, XeSS 40.2 -> 46.2; all three now beat the plain stretch in the turn, and a slow pan is unchanged.
  On by default; "Steady in fast motion" in each upscaler's panel turns it off to compare. XeSS also gets the motion's distrust mask now
  (as its responsive mask), as FSR and DLSS always did.
- **The DLSS model list**, one line each: K (NVIDIA's default), **L (recommended)**, J, M (DLSS 4.5) and E (DLSS 3's CNN). On the same
  recording L is the only one that does not drift from the picture in a slow pan, and close to the best in fast turns; E is the best in
  fast turns and the lightest; M the weakest here and the heaviest.
- **The motion estimate on a character in a fast turn.** A character the camera follows (dark, faintly textured) was held to the guess of
  the background sweeping past and took random vectors; the pull toward the guess is now capped, so it finds its own. For the upscalers,
  Neural Rendering and frame generation alike.
- **Neural Rendering's Temporal smoothing no longer trails** in a fast turn: the last frame's change is held within the range of this
  frame's own around each pixel (as temporal anti-aliasing does), and the smoothing fades out in fast motion. (Off by default.)
- **HDR:** the frame's light is clamped at 0 before the upscalers (an HDR10 colour outside Rec.709 came out negative).
- **DLSS Upscaler and Neural Rendering together:** switching Neural Rendering off while the DLSS Upscaler ran shut NVIDIA's NGX down under
  it ("FeatureNotFound" in the log) and the upscaler switched itself off. NGX now shuts down only when the last of our addons using it
  does, and the DLSS Upscaler starts NGX again if its feature is lost anyway.
- **Frame generation of our own (the FSR Upscaler; a prototype, not in this release: only in a test build, CMake NR_FRAMEGEN=ON)** from its first live tests: FSR 3.1 frame generation is
  dispatched directly (its own swap chain paced itself and, with our pacing, halved the real frame rate, 30 -> 18); the frame between and
  the real one go out evenly spaced; HDR frames are measured in their SDR view; a guard takes back what FSR pastes over a character in a
  turn ("Keep the character clean in turns"); what it did is logged every 10 s.
- **The recorder** can keep what is shown ("Record what is shown", with frame generation of our own, in a test build): the frames at the screen's size,
  the frames made between and the real ones, each marked (`nr_lsrec export` names them).
- **Tools:** `nr_sreval` scores an upscaler offline on a recording (the addon's own engine: DLSS, FSR or XeSS, any setting); `nr_fgeval`
  gains the live gap, a score around the character, FSR's debug views, direct dispatch and the addon's own frame generation engine.
  Findings in `docs/frame-generation-research.md`.
- **Tests:** targeted runs of the upscaler and Neural Rendering scenarios (moving pictures on all three upscalers, sharpening, edges, HDR,
  smoothing), and offline measurements with `nr_sreval` (now with a steadiness score: shimmer and flicker from frame to frame) and
  `nr_fgeval`. Live: the DLSS Upscaler in World of Warcraft (the owner: happy with it). Not yet tried live: Neural Rendering's changes and
  the NGX fix above.
- **Known:** some flickering while moving with the DLSS Upscaler in World of Warcraft (noted, to look into).

## 0.9.12 (2026-09-27)

A third upscaler, Intel XeSS, and a release half the size.

- **The XeSS Upscaler.** Intel XeSS Super Resolution in place of Lossless Scaling's NIS pass, beside the DLSS and FSR Upscalers (only one
  of them runs at a time), off until switched on. It runs on any DirectX 12 card with Shader Model 6.4: Intel Arc (on its matrix units),
  NVIDIA and AMD. Intel's runtime, `libxess.dll` 2.0.2 from the XeSS 3 SDK (signed by Intel; the same file OptiScaler ships), comes with
  it, with Intel's licence. It uses the same motion measured from the frames, picks the XeSS quality setting that takes the game's size
  by itself, runs HDR games in its HDR mode, and has the upscalers' sharpening, edge smoothing and picture controls. Tried in the test host
  (upscaling, a sliding picture, HDR highlights, a 4:3 window); not yet in a game.
- **The release zip holds nothing twice.** It carried every file loose and again inside `LSAddonManagerSetup.exe`; now only Setup, the
  install note and the licences. With XeSS's 57 MB runtime included it is 111 MB (0.9.11 without it: 113 MB). To install by hand,
  `LSAddonManagerSetup.exe --extract files` writes the files into a new folder and changes nothing else (INSTALL.txt and the README say how).
  The manager's updater only ever used Setup from the zip, so updating works as before.
- **Tests:** five XeSS scenarios. Told that nothing moves while the picture slides, XeSS drifts in average colour where DLSS and FSR do
  not; with the measured motion it follows the picture as the others do.

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
