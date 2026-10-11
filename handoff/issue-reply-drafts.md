# Replies to the open issues (drafted 2026-10-09). ALL POSTED on 2026-10-11 at the owner's go-ahead (#11, #14, #16, #18, #19); kept as the record of what was said.

## #19 DLSS Upscaling Low Perf? (dual GPU, 5070 shows the picture, 5080 renders, 2.5k1k -> 5k2k)

Measured here with `nr_sreval bench=200` (RTX 4070 Ti SUPER, an HDR clip, 2x upscale to 2562x1442 = 3.7 MP), the GPU time of one upscaled frame: **DLSS 1.35 ms, FSR 3.1 0.70 ms, XeSS 1.24 ms**
(of which the motion estimate 0.20 ms). The cost follows the output's pixel count, so 5120x2160 (11.1 MP) is about 3x: **DLSS about 4.0 ms, FSR about 2.1 ms, XeSS about 3.7 ms per picture**,
on a card as fast as that one; a 5070 is somewhat slower.

The upscaler runs on **every picture Lossless Scaling presents**, the generated ones as well as the real ones. With frame generation x2 on a game at 120 fps that is 240 pictures a second:
240 x 4 ms = 0.96 s of GPU work in each second, before frame generation itself, so a card that is "taken to 100 %" and falls to 87 real fps is what the numbers say, not an overhead. Plain
NIS (and LS1) cost a fraction of a millisecond, which is why they sit at 20 %.

What brings it down, in order of effect:
1. **FSR instead of DLSS** (about half the cost at this size).
2. In the DLSS/FSR panel, **"Lighter upscaling of generated frames (test)"**: the real frames get the full upscale, the generated ones a cheap picture (about 0.06 ms of the above), which takes roughly half the load at x2.
3. A lower frame-generation multiplier, or a cap on the game (a frame cap with some headroom helps most).
4. The game rendering at a size nearer the output (the cost is the output size, not the input size).
If it is still slower than these numbers say, please attach `logs\DLSS4DLAA.log` from a short session and the panel's "Last second" line.

## #11 FSR4 Crashes on RDNA2 (confirmed no crash since 0.9.32; "no discernible image difference")

Good to hear it no longer crashes. The log you posted says `the runtime runs 4.1.1b (it holds: 4.1.1b, 3.1.5, 2.3.4)`, so FSR 4.1.1b is the runtime that is upscaling. That the picture looks like NIS at x1.00
is expected: the log shows `1920x1080 -> 1920x1080 (x1.00)`, i.e. no enlargement, and without the game's camera jitter FSR can only anti-alias and steady the picture (see the answer in #10). The difference shows at
a real upscale (a 1080p window on a 1440p or 4K screen) and with **Ctrl+Shift+F7** (before / after) on a scene with fine detail. If you can, try that and tell us what you see.

## #14 NR resolution capped at 1080p

There is no 1080p cap. The line `3840x2160 -> 1920x1080` in the Performance section is **the frame -> the model's input**: the model works on a smaller picture and its change is stretched back to the full 4K frame.
The size is chosen by **Model resolution** (a slider) and, when **Auto quality** is on, lowered further to keep the model inside its time budget. Turn Auto quality off or raise its budget and floor for a larger input; it costs
more GPU time (nitro's 12 ms on a 4090 is a model input near 4K). The panel now says this under that line (next release). If the line still says 1080p with Auto quality off and Model resolution at 1.00, please attach
`DLSS5NR01.log` (the lines with `auto:`) and a screenshot of the Quality section.

## #16 "it looks like NIS but older versions don't"

Thank you for the report. Two things were broken in some builds between 0.9.28 and 0.9.34 and could give exactly this: Auto quality was off on a **new install** (a setting that should have been on), and the **XeSS
upscaler was missing from the zip** (0.9.28 to 0.9.33). Both are fixed in 0.9.35 and later; the current release is 0.9.38. Please update, then check the upscaler's panel: the **Upscaling** line says whether it is
actually replacing NIS (and if not, why), and `logs\<addon>.log` says "NIS stays" with the reason. If it still looks like NIS on 0.9.38, attach that log and say which Scaling Type and window layout you use.

## #18 terrible ghosting in Expedition 33 (NR + frame generation)

Thanks for the details (lowering local contrast removes it, and the second-frame setting helps). That points at the model's change being applied to the generated frames with the wrong motion. Please attach
`logs\DLSS5NR01.log` from a short session where the ghosting shows, and say your settings (Local contrast, Ghost guard, the frame-generation multiplier, game fps). With those I can check whether the ghost guard
is engaged for your motion. Meanwhile **Ghost guard on** and the lower local contrast you found are the settings to keep.
