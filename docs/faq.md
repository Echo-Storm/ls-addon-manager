# Questions and answers

Short answers to the things people ask most. The [README](../README.md) has the overview and a troubleshooting table; Neural Rendering has its own [user guide](../addons/DLSS5NR01/docs/user-guide.md) with a longer troubleshooting section.

## Before you install

**What does it actually do to my Lossless Scaling?**
Lossless Scaling loads a file called `Lossless.dll`. LS Addon Manager takes that name; Lossless Scaling's own file is kept as `Lossless_original.dll`, and the manager passes everything on to it. Nothing in Lossless Scaling is patched
on disk, and uninstalling puts its own file back.

**Does it touch my game?**
No. It runs inside Lossless Scaling, not inside the game. (Whether a game's anti-cheat has an opinion about Lossless Scaling itself is between you and the game; the manager does not change that, and cannot promise anything about it.)

**Which versions and games has it been tested with?**
Lossless Scaling **3.2.2.0** on Windows 11 with an RTX 4070 Ti SUPER, and the game **World of Warcraft: Forever** (the beta). Other versions, cards and games are untested. If you try one, tell us how it went.

**Steam or non-Steam Lossless Scaling?**
Either. Setup looks in Steam libraries and in usual places on every drive, and lets you pick any folder. The copy it was developed with is not from Steam.

**Is it free? Is it safe to download?**
It is free and MIT-licensed, and the source is in this repository. The files are **not signed**, because signing costs money and free signing for open-source projects needs an application; Windows SmartScreen and some antivirus programs therefore
distrust them. You can build everything yourself from the source (see the README).

**Does it send anything over the internet?**
Once a day it asks github.com for the latest release number: GitHub sees your IP address and the program's name and version. It downloads a release only when you choose **Download and install** (checked against GitHub's SHA-256 before it is used), and never downloads anything else. You can turn it off in *Settings > Updates*. Setup itself uses no network at all.

## Setup

**I changed a file in an addon's folder (or put a newer runtime there). Does an update overwrite it?**
It depends on the file. Setup replaces **every file of ours** on an update (the addons' DLLs, `addon.json`, icons, licences), a test build of one included; the old file goes to `backups`. The one thing it leaves alone is a **vendor runtime you replaced with your own** (a newer DLSS, FSR or XeSS file in an addon's `dlss`, `fsr`, `xess`, `vfx` or `runtimes` folder): Setup keeps a record of what it put there, and a file that is no longer that is yours. It says which it kept. To get our copy of such a file back, delete or rename yours and run Setup again (an update or **Reinstall**). An addon card that says **OLD BUILD** has a DLL older than the manager: run Setup again (Reinstall) to replace it.

**Setup says Lossless Scaling is running, but I closed the window.**
Make sure it has really quit: look for its icon in the notification area, and for `LosslessScaling.exe` in Task Manager, and close it there. Then press *Check again*.

**Setup did not find my folder.**
Choose *Use a different folder...*, then *Browse for the folder...*, and pick the folder that holds `LosslessScaling.exe`. If you pick the folder above it, Setup uses the Lossless Scaling folder inside it when there is exactly one. Setup remembers your choice.

**Why does Setup want administrator rights?**
Only when the folder is somewhere Windows protects, such as under `C:\Program Files`. Otherwise it runs as you. If you would rather not, use a copy of Lossless Scaling outside `Program Files`.

**I updated Lossless Scaling and the manager is gone.**
Lossless Scaling put its own `Lossless.dll` back. Run Setup: it offers *Repair*, keeps the new original, and puts the manager back. Your settings are untouched.

**Can I undo it?**
Yes. *Uninstall* in Setup puts Lossless Scaling's own file back (your addons and settings stay, or go to the backups folder if you choose that). Everything Setup replaces is copied to `backups\` inside the Lossless Scaling folder first.

**Can I run Setup from a script?**
Yes: `LSAddonManagerSetup.exe --silent install --folder "<folder>"` (or `uninstall`, `status`), with `--log <file>` to get the messages. The exit code is 0 when it worked.

## Using the manager

**Where did the window go?**
It hides to the notification area when you close it (Windows may put the icon under the ^ arrow). Click the icon, or press **Ctrl+Shift+F12**. *Settings* lets you change the key and choose whether the window opens when Lossless Scaling starts.

**Where are my settings?**
In `addons\config.json` inside the Lossless Scaling folder. *Settings > Save settings to a file* saves everything (the manager's and every addon's) to one file you can load back later or on another PC.

**A log line says `[Crash] unhandled exception` and Lossless Scaling closed.**
The manager writes a backtrace when the process it lives in crashes, so that the crash is not a mystery; it records the crash, it does not cause it. Frames in `coreclr.dll` are Lossless Scaling's own .NET code. Send the log with a bug report (below) if it happens again.

**Why is the Performance tab's GPU data missing?**
GPU load, power and temperature come from NVIDIA's driver library, so they only exist on NVIDIA cards, and are only read while the tab is open.

**Windowed mode or ReShade passthrough does nothing.**
Both are switched off until you turn them on in the *Features* tab. Windowed mode's virtual display has to exist before Lossless Scaling starts, so restart Lossless Scaling after switching it on.

## Recording and the GPU limit

**How do I record a clip for a bug report?**
Open the manager's **Features** tab and switch **Recording** on. Play, and when the problem has just happened press **Ctrl+Shift+F1** (you can choose any key from F1 to F12): you hear a tick at once and a rising pair of tones when the file is saved (a low tone means there was nothing to save). The clip is a `.lsrec` file with a note beside it (`<file>.lsrec.json`), in `Videos\Lossless Scaling` unless you chose another folder. No addon has to be on, and it works with Lossless Scaling's frame generation on or off. Attach both files to your report.

**The clip is big. What can I do?**
*Compact* (the default) already stores HDR frames as an 8-bit picture, about a sixth of the size: ten seconds at 1440p is about 1 GB. *Exact* keeps every frame as captured and can be 8 GB for the same ten seconds. `tools\pack_recordings.ps1` compresses saved clips further, after the fact and checked.

**What does the GPU limit do?**
It keeps the graphics card under a load you choose (80 to 100 %, 95 % by default) by making the addons' work cheaper while the card is at or over it: Neural Rendering's Auto quality takes the model's working size down, and the upscalers' lighter mode for generated frames starts. When the load is well under the limit they go back. It never changes the game or Lossless Scaling, and it is off until you switch it on in the Features tab.

## DLSS 5 Neural Rendering

**Where do I get `nvngx_dlssnr.dll`?**
This project does not distribute it, does not download it and does not say where to find it. You provide your own copy, and put it next to `LosslessScaling.exe` (Setup and the addon's *Browse for the model file...* button can copy it there). Everything else works without it.

**What does "Test compatibility" tell me?**
It runs your model file once, in a separate program, on your graphics card, and says whether it loads, creates its feature and changes a test picture. It is the way to find out before playing that a model build cannot run on your card. It does not prove the picture looks right in every game.

**Can I share what worked on my card?**
Yes, please: see [model-compatibility.md](model-compatibility.md).

## DLSS, FSR and XeSS Upscalers

**The upscaler is on, but nothing changes.**
In Lossless Scaling choose **NIS** as the Scaling Type: the upscalers take the place of that pass. The addon's panel says what it is doing,
and when something is in the way, what (a card that is not NVIDIA's for DLSS, a frame format, a window layout). Ctrl+Shift+F7 switches
between the upscaler and NIS while you play.

**Why is the difference smaller than in a game with DLSS built in?**
A game with DLSS built in moves its camera by a fraction of a pixel every frame, and DLSS combines those frames into detail finer than the
game renders. Lossless Scaling only sees the finished frames, without those shifts, so the upscalers work as a very good upscaler and
anti-aliasing rather than adding new detail. The difference to NIS shows most in motion, on fine detail and foliage, and when the game
renders well below the screen's size (2560x1440 to 4K, for example); on a clean, low-detail game it is smaller. The
[upscalers' guide](../addons/DLSS5NR01/docs/upscalers.md) has the settings that help most (the model, sharpening, stability).

**DLSS, FSR or XeSS?**
On an NVIDIA RTX card try DLSS and FSR: DLSS smooths edges better, FSR 3.1 costs less and keeps text crisper, and FSR 4 is the best in
motion but costs the most. On any other card, FSR, and on Intel Arc also XeSS, which runs on the card's matrix units there. XeSS is new in
0.9.12 and not yet tried in a game: reports are welcome.

**My frame rate drops a lot with DLSS or FSR 4 and frame generation on (and not with XeSS).**
With frame generation the upscaler runs on every picture shown, the generated ones too, so a heavy model (DLSS 4.5, FSR 4) can take far more of the card than a light one. Since 1.0.1 the upscalers lighten the generated frames by themselves while the graphics card is at its limit (the manager's GPU limit, or 95 % while that is off): the game's own frames are still upscaled in full. It is on by default; *Lighter upscaling of generated frames when the card is full* in the upscaler's panel turns it off, and the log says when it starts ("the card is at N % ..."). A frame-rate cap with some headroom, or a lighter DLSS mode, helps further.

**Do the addons take long to start?**
The first time after an update they compile their shaders (1 to 5 seconds); since 1.0.1 the compiled code is kept in `%LOCALAPPDATA%\LSAddonManager\shader-cache` (about half a megabyte; files unused for 90 days are removed), so later starts skip it. Deleting that folder is safe: it is made again. `NR_SHADER_CACHE=0` in the environment turns it off.

**What is Video Super Resolution, and when do I use it?**
It is NVIDIA's RTX Video Super Resolution in place of NIS: a trained network that sharpens what it upscales, one picture at a time. It is made for **movies, images, text and interface**: a film in your player, photos in a viewer. It works on the whole picture, and a picture that has not changed (a still image, a paused video, a film at 24 or 30 pictures a second on a 60 Hz or faster screen) is worked on once, not for every refresh. A game that moves a lot is better served by the DLSS, FSR or XeSS Upscalers. It needs an NVIDIA RTX card; the NVIDIA files it uses come with the addon. Choose NIS as the Scaling Type and show the picture in a window smaller than the screen. The [guide](../addons/DLSS5NR01/docs/vsr.md) has the settings and costs.

**Does it work with HDR games?**
Yes: 16-bit (scRGB) and 10-bit (HDR10) frames are upscaled in the upscaler's own HDR mode (since 0.9.10) and go back as HDR. If a game looks washed out or too dark,
set *Frame encoding* under Upscaling by hand.

## Capture cards and 30 fps games

**The panel says the picture Lossless Scaling captures repeats.**
A 30 fps game seen through a capture card at 60 or 120 Hz (or a video at 30 fps on a 120 Hz screen) reaches Lossless Scaling as the same picture two or four times in a
row. Frame generation then makes its pictures between identical frames and the motion advances unevenly, which looks like judder. The addon only notices and says so
(it needs frame generation on, and says nothing for a still scene or a game at its real rate). The fix is on the source's side: set the capture card's or the capture
program's signal to the game's real rate (30 for a 30 fps game), then use frame generation x2 or x4 for your screen. More in
[the research note](capture-card-sources.md).

## Questions people ask (from the community)

**Will it get me banned in an online game? Does it "inject" into the game?**
No injection into the game. The manager is a library that Lossless Scaling itself loads (it stands in for Lossless Scaling's own `Lossless.dll`), and its hooks are inside **Lossless Scaling's process only**: they see the pictures Lossless Scaling has already captured, as OBS or any capture program does. Nothing is written into the game's process or files, and the game cannot see that the manager exists. Whether a particular game's anti-cheat accepts Lossless Scaling at all is for that game to decide (if the game allows Lossless Scaling, nothing here adds to it), and no one can promise it for you. If the game's rules forbid overlays or capture tools, don't use it there.

**VirusTotal / Windows Defender says it is malicious.**
See [antivirus.md](antivirus.md). In short: the files are unsigned and new, and a library that replaces another library and hooks calls looks like malware to a heuristic scanner, even though it does neither of the harmful things. The source is in this repository and you can build it yourself.

**How much delay does EchoFG add compared with Lossless Scaling's frame generation? Can Reflex help?**
EchoFG shows each real frame later by the share of a real frame that the frames between take (half of it at x2, three quarters at x4) plus the making time: about 10 ms at 60 real frames a second x2, about 22 ms at 30 x2 (the panel shows the figure for your case). That is the least any generator that makes frames *between* real ones adds, Lossless Scaling's included, because a frame between two real ones needs the later one. A higher real frame rate makes it smaller. Reflex works inside the game, before the frame reaches Lossless Scaling; it cannot lower what is added afterwards. Only guessing ahead (extrapolation) would avoid the delay, at a cost in picture quality.

**Why is the upscaler so expensive with frame generation on?**
Lossless Scaling's scaling pass, which the upscalers replace, runs on every picture it shows, the generated ones too. With x2 a heavy model would run twice per game frame. So the generated frames get a lighter picture by default (the last upscaled frame moved along the motion, checked against the new frame, sharpened), and the **Performance** preset goes further. The **Quality** preset upscales every picture in full. With EchoFG as the frame generator, the upscaler runs once per real frame, because EchoFG makes its frames after the upscale.

**My card is an RTX 20 or 30 (or GTX 16): DLSS is slow.**
These cards have no FP8 in their tensor cores, and DLSS 4.5's models (L, M) cost them several times what DLSS 4's model (K) does. The Auto setting therefore picks K on them (L on RTX 40 and 50). If you chose a model by hand, K is the light one.

**Which GPU does what with two graphics cards?**
Everything Lossless Scaling does runs on the card you set as its *Preferred GPU*: the capture, its frame generation, the scaling pass, and so the upscalers, Neural Rendering and EchoFG, which use that card. The game runs on the other. See [dual-gpu.md](dual-gpu.md). A weak second card is the usual limit: the status line of an upscaler shows its cost in milliseconds and the video memory it has taken, and the Performance preset lowers it.

**Where does it get motion vectors? (It is not in the game.)**
It estimates them from the pictures themselves: the motion between the last two frames Lossless Scaling captured, found per patch of the picture by our own shaders (the "motion estimate" in the logs). That is why it works with any game, and why fast turns, repeating patterns and things that appear from behind others are its hardest cases.

**Is there a Linux version?**
No. Lossless Scaling is a Windows program, and the manager is a library it loads. (On Linux, `lsfg-vk` runs Lossless Scaling's frame generation as a Vulkan layer; it is a different project and unrelated to this one.)

**Is it worth it on an older card (GTX 1650, an RX 6400)?**
FSR and XeSS are the choices there; DLSS needs an RTX card, and DLSS 5 / Neural Rendering needs a strong one (it halves the frame rate even on fast cards). On a card that is already at its limit, any extra pass costs frames: try the Performance preset, a lower flow scale in Lossless Scaling, or leave the upscaler off.

**Lossless Scaling's frame rate counter shows the wrong number with EchoFG.**
Fixed in 1.0.5: with EchoFG the counter reads "captured / shown" (for example 60 / 120). On a Lossless Scaling build the manager does not know, EchoFG draws a small counter of its own instead.

## Reporting a problem

Open an issue at <https://github.com/Echo-Storm/ls-addon-manager/issues>. Attach the **diagnostics file** (*Settings > Create a diagnostics file*): a zip of your logs and settings that is only made on your PC and uploaded nowhere. Say what you did, what you expected, what happened, your Windows version, graphics card and Lossless Scaling version.
