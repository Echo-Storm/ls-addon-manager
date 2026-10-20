# What the MFG Super Motion engine does, as far as its own text shows, and where EchoFG stands (2026-10-20)

Second look at the 1.0.2 release folder (`NEW FILES/research`, kept out of git), after the first study (`docs/mfg-super-motion-study.md`) looked at the vendor-FG side only. The engine (`Engine/nvngx.dll`, an executable) carries the source text of its own generator's compute shaders as readable strings (about 140 000 characters; their comments say what each pass is for and which test case taught it). This page is **ideas written down in our own words**, to be tried and measured on our own code. No code was copied or kept in the repository; the extracted text stays in the ignored research folder. The engine itself was not run.

## Their generator ("MSM FG 1.1"), in the order it works

1. **Brightness pyramid, filtered before it is halved.** A brightness plane at the analysis size (nine taps a third of a texel apart, the middle ones counted more) and then smaller sizes with a 1-3-3-1 style filter. Their comment: what is finer than a size must be gone from it, or it shows as a pattern of its own that moves its own way. (Ours: a plain pyramid; worth a look.)
2. **Block search at 2 x 2 texels, both ways.** The frame before is searched into the frame after *and* the frame after into the frame before, each block compared over a 4 x 4 neighbourhood. Smallest size: every place within 4 texels of "not at all" and of where the picture as a whole is expected to have gone, with a cost for every texel further from the nearer of the two. Larger sizes: the nine blocks below offer their words, then one texel further around the best; leaving the word of the block below costs as much as that word counts, and more the less the block itself shows (a soft road matches a long way off as well as where it is).
3. **A "sure" value per block**: it shows something (contrast) and it was found (its best match is close to the best possible for its contrast). A plain block's own word is not trusted; the blocks around say it.
4. **Relax**: a block takes a neighbour's word where that explains it as well, weighted by how sure the neighbours are; its own word wins a tie.
5. **Far search for thin, fast things** (a lamp post seen from a car): at the second largest size, a block that was not found is searched far and wide (every second texel within a reach), then one texel around the best, and accepted only where it fits clearly better.
6. **Cross-check of the two ways**: each block's way is carried to where it lands in the other frame; a block there is offered the way back of the block that landed on it and takes it where it fits clearly better. Then each block gets a **found** flag: it matches within what its contrast allows *and* the way back from where it lands comes home within a texel plus a bit per texel of its length.
7. **Sub-texel** from a parabola through the match errors one texel either side (cheap, only at the end).
8. **The picture at large**: for each tile, the dominant movement of the blocks that were found and show something (a mean, then a mean weighted toward those near it, so a thing of its own in the tile is left out), and how many blocks had a say. A block that shows little goes by the picture at large around it, the more so the less it shows (a repeating face, such as a tower's fine upright lines, takes the picture's word when the two real frames agree under it about as well as under its own).
9. **Marks (HUD) as a per-pixel count over time.** A pixel that is alike in both real frames says nothing by itself (a soft picture has many); what says it is that drawn the way its place goes it would come out clearly another colour *and both frames show it the same*. Counted up by two where that holds, down by one where the pixel goes with its place, nothing learnt where its place does not move; a count over a threshold is a mark. The game's read-outs learnt in earlier sessions (their "Adaptive Motion Learning") pre-set the flag. A mark is drawn held as the frame before has it **only if the frame after has it the same** (brightness test), so a piece of wall wrongly taken for a read-out is drawn moving with the rest.
10. **The word field, made smooth**: a place whose word stands alone (seven of the eight around agree with each other and it is further than that from them) takes theirs; then every word becomes the mean of the 25 around it weighted by how near each word is to its own and by whether both frames show it. No steps between places.
11. **Direct drawing ("round 40", the mode they ship for the vendor-style path)**: the word of a place is the movement found *at that place itself*, forward from the frame before and backward from the frame after, the mean where the two agree and the one found where only one was; the pixel is then drawn straight from both real frames at the moment asked (`A(x - t v)`, `C(x + (1 - t) v)`), mixed by the moment. **Nothing is carried and nothing is chosen between**: their log says carried-and-resolved frames made a "puzzle" (a piece of picture landed somewhere else) and a wrong word now comes out as a soft trace where it is. Where the two warped samples differ by more than a threshold, the pixel leans to the nearer frame instead of a double picture.
12. **Weighted blend of the four nearest words** (older path): each word's own pixel weighed by how near the place is and how well the two frames agree under that word (exponentially in the misfit), so where one fits clearly best it wins and where they fit alike they mix: soft, never a square. And where the place's word does not make the frames agree, the picture-at-large word is tried and the two results are mixed by fit.

Around it (not shaders): a presenter on top of the game window with a queue target, each pair shown at its own moment, a measured desktop-to-screen latency, a retry at a lower multiplier when refused, VRR checks, and "ordinary output retained" as the fallback whenever anything fails.

## Where EchoFG stands, point by point

| Their step | EchoFG today | Offline test result / plan |
|---|---|---|
| 1 pyramid filter | plain pyramid (FlowEstimator) | try the weighted filter; scorer: `tools/flowlab` (photometric error, interpolation PSNR) |
| 2 both ways | one flow (at the later frame); the other way by splatting and reversing | **tested**: a second real flow (A to C) and the direct draw below |
| 3-4 sure / relax | distrust mask, 3 x 3 vector median, per-pixel pick among the block's and the three nearest blocks' vectors | try the contrast-weighted relax |
| 5 far search | none beyond the coarse level's range | board E15 |
| 6 cross-check, found flag | none | board E12 (already a card): the way-back check and "best neighbour that comes home" |
| 7 sub-texel | refined at half size | fine |
| 8 picture at large | a global-shift test at the smallest level only | board E16: per-tile dominant motion as the fallback for plain blocks and repeating textures (E2's "blocky cells on periodic textures") |
| 9 marks over time | per-frame still detection (`keepstill`), no memory | board E11 (already a card), now with their exact recipe |
| 10 smooth word field | 3 x 3 median | board E17 |
| 11 direct drawing | splat + hypotheses + holes + soften | **tested, see below** |
| 12 fit-weighted blend | per pixel the best of the 2 x 2 nearest cells' vectors by fit | roughly equal |
| presenter | `nr_fgpresent` / EchoFG's own pacing (E3: the generated frame's present blocks 17-20 ms) | board E3 |

## The measured result that matters

Same recordings and the same 12 pairs (clip frames 0-24 of three segments; the real frames every second frame, the truth in between; PSNR gain over the plain blend of the two real frames, `tools/flowlab/synth_lab.py` and `nr_fgeval dumpraw`):

| segment | FSR 3.1 | **EchoFG's synthesis** | trivial one-field warp | **direct, from both fields (no carrying)** | direct + fit choice + soft |
|---|---|---|---|---|---|
| 23-16-58, camera moving | +1.71 | +2.09 | +1.76 | **+2.12** | +2.15 |
| 23-18-17, sunset | +2.10 | +2.23 | +2.06 | **+2.22** | +2.15 |
| 23-16-37, building | +1.72 | +2.00 | +1.78 | **+1.92** | +1.83 |

- **A direct draw from two real flow fields, about thirty lines of arithmetic with nothing carried, matches EchoFG's whole synthesis** (splat with atomic minimum, two hypotheses, holes, soften): within 0.1 dB either way. A fit-based choice among candidate words, and a soft lean to the blend, add nothing measurable on these clips.
- So **EchoFG's synthesis is not where its quality is decided** on this footage; the motion field is (as the optical flow study found), and probably the cases this scorer does not see: HUD and read-outs (marks), thin fast things, repeating textures, and the pacing on the real screen.
- **A cost opportunity**: the direct draw needs no splat buffers, no atomics, no hole pass. If it holds up on the other clips and in motion it is a simpler and cheaper synthesis at the same quality (board E18). It does need the second flow (A to C): a second estimator run per pair, or the cheaper way-back check their step 6 does.

## What this cannot say

- It cannot say whether MFG Super Motion's generator is "way better than LSFG" or than EchoFG: the engine was not run (it only works as a capture program on a game window), and LSFG is closed. The numbers above are for *our* flow in *both* syntheses. A fair comparison needs the same game scene recorded from the three generators' outputs (Spout/OBS for theirs, our recorder for LS) and judged by eye and by a scorer; that is a live job (board E19).
- The clips are 33 ms apart at 1440p cropped to 720p with a camera moving in WoW; fast flicks, 4K and HUD-heavy scenes are not in them.
- The pictures-at-large, marks and far-search ideas are only reasons to expect gains where this scorer is blind; each needs its own test clip (a HUD over moving background; a lamp post past a wall; a tower face), which the recordings of 2026-10-19 do not contain.

## Order of work (proposal)

1. **E18** the direct draw as an alternative synthesis in `own_synth` (same flow, a second flow or the way-back check): measure on all clips with `nr_fgeval`, then cost on the card.
2. **E11** marks as a per-pixel count over time, with the "held only if the next frame has it the same" test: needs a clip with a HUD over moving ground (record one: Features > Recording in WoW with the action bars in view).
3. **E12 / E16 / E15** flow: the way-back check, the picture at large per tile, the far search, each scored by `tools/flowlab/score_flow.py` and the synthesis scorer.
4. **E3 / E19** pacing and the live A/B.

## First result of the work: marks in EchoFG (2026-10-20)

Step 9 (marks) was built into `own_synth` (three small passes per pair: the moving part of the picture, the picture at large around each cell, and the per-cell counts that live from pair to pair; the synthesis holds a marked pixel as the plain mix of the two real frames). Offline lab first (`tools/flowlab/hud_lab.py`: a simple version recovers a HUD from 46 to 56 dB of a 57 dB ceiling on the turning clip), then in the real synthesis, scored with `nr_fgeval dumpraw=... dumpx=... dumpy=...` and `tools/dlssg_probe/score_dump_hud.py`:

| clip (2560x1440, EchoFG synthesis, GPU) | HUD pixels, marks off | marks on | the rest of the picture, off / on | whole frame, off / on |
|---|---|---|---|---|
| 23-16-58 (camera turning), chat and action bars region | 37.6 dB | **51.5 dB** | 24.41 / 24.46 dB | 25.82 / 25.93 dB |
| 23-16-37 (building) | | | | 27.71 / 27.74 dB |
| 23-17-18 (calm canyon) | | | | 38.74 / 38.60 dB |
| 23-17-55 (dark interior) | | | | 38.59 / 38.49 dB |

- **The HUD that was dragged along with the turning picture is now held: +14 dB on its pixels**, with no loss on the rest of the picture there, a small gain on the whole frame of that clip, and -0.10 to -0.14 dB on two calm clips. The GPU cost is about 0.2 to 0.6 ms a pair at 2560x1440 (noisy measurements).
- Tuning, kept so it is not redone: looser settings (alike under 12 levels, "changed" over 30, count of 4, held pixel under 84) reached 55 dB on the HUD but cost 0.25 to 0.4 dB on the calm clips (flat dark areas that merely look alike were held); the tighter ones in the code (alike under 6, changed over 60, count of 4, cell under 42, pixel under 60) keep most of the gain.
- The slow walking clip (23-16-25, the quest tracker, chat, bars and minimap over moving ground) shows almost nothing either way (HUD 70 dB in all methods): a HUD is only damaged when the picture moves a lot between frames, which is when it matters.
- `NR_SYNTH_MARKS=0` in the environment turns the marks off for comparing. EchoFG stays a test build; the presenter battery (`dev.py presenter`) passes with it.
- Not done from step 9: pre-seeding marks from per-game learnt read-outs, and a clip with a HUD over a *fast* moving scene at the edges of the picture (the dump pieces above were the bottom-left and bottom-right; the quest tracker strip is still to be looked at by eye).

## Result of E18: the direct draw inside EchoFG (2026-10-20)

Built into `own_synth` as a second mode (`Record(..., motionReverse)`; `nr_fgeval ... owndirect=1`): the motion estimate is run a second time on the pair the other way round (the flow at the earlier frame's pixels), each pixel of the frame made takes the movement found at that pixel from both fields (their mean where they come home to each other, else the one under which the moved pair agrees better) and is drawn straight from the two real frames; the marks hold applies as before; no splats, hypotheses or holes. Same recordings, 29 frames each (PSNR of the frame made, dB):

| segment | splat synthesis (today) | direct draw |
|---|---|---|
| 23-16-58 camera moving | 25.93 | 25.84 |
| 23-16-37 building | 27.74 | 27.65 |
| 23-18-17 sunset | 30.10 | 30.05 |
| 23-17-18 calm canyon | 38.60 | 38.84 |
| 23-17-55 dark interior | 38.49 | 38.32 |
| 23-16-25 walking, HUD | 45.05 | 45.12 |

- **Quality is the same** (mean difference -0.01 dB; +0.24 on the calm clip, -0.17 on the dark one), as the lab said.
- **The cost does not pay.** The motion estimate costs about 4 to 5.5 ms a pair at 2560x1440 on this card (pyramid 0.4-0.9, search 2.0-2.7, pixels 1.5-2.1; the measurements are noisy, the card's clocks move), so **the second estimate adds about as much as the whole estimate again**, far more than the splat synthesis it replaces (about 0.7 ms best, 2 ms median). The direct pass itself was not cheaper than the splat's Synth either (2.7-4.5 ms median for a full-resolution pass with the marks lookup). The splat pipeline is, in effect, the cheap way to get the second field (it carries the one estimate's vectors onto the other frame and reverses them).
- **Not adopted**; the mode stays in the code as an experiment (`owndirect=1`), not used by EchoFG. Where it could pay: a way-back field that costs less than a second estimate (their step 6 derives the check from the first estimate's own vectors), or a "light" synthesis from one field (the lab's one-way draw: about 0.3 dB below EchoFG's synthesis) for the GPU-limit use case, if the splat passes turn out to be the cost there.
- The estimate itself is the large cost: **4 to 5 ms of the pair's budget at 1440p**. Its search stage is half of it. The coarse-seed idea from the optical flow study (E14) now has a number to beat.

## Result of E12: the way-back check on our flow (2026-10-20)

Lab only (`tools/flowlab/check_lab.py`, three segments, 8 pairs each): each pixel's vector is checked by the other field at its end point (a right vector comes home within 1.5 px plus a tenth of its length); a pixel whose vector does not come home takes, of 33 neighbours' vectors and none, the best one that does and that carries its 3 x 3 surroundings within 15 levels of what its own did (or keeps its own where none does). About 11-15 % of the pixels were replaced.

| draw | gain over the blend, without the check | with the check (a real second field) | with the check (the way back made by splatting the forward field alone) |
|---|---|---|---|
| one field, 23-16-58 | +2.20 dB | +2.29 | +2.27 |
| one field, 23-18-17 | +1.69 | +1.76 | +1.75 |
| one field, 23-16-37 | +2.03 | +2.15 | +2.17 |
| both fields (direct draw), the three | +2.53 / +1.79 / +2.17 | +2.54 / +1.82 / +2.25 | n/a |

- **The check helps a little: +0.06 to +0.14 dB on a draw from one field, +0.01 to +0.08 on the direct draw from both**, and the cheap way back (the forward vectors splatted onto the other frame and reversed, which EchoFG's pipeline already does) gives nearly all of what a real second estimate gives.
- **Not built into the estimator**: a candidate search of 33 neighbours per pixel (plus the splat) is a pass worth perhaps a millisecond at 1440p, for about a tenth of a dB, and EchoFG's synthesis already picks between two hypotheses per cell using the same splat, so its gain there would be smaller still. Kept as a measured option (board E12 done, "marginal").
- What this leaves as the open levers on the flow: the picture at large for plain and repeating blocks (E16), the far search (E15), the smoothing of the field (E17), and the cost of the estimate itself.

## Results of E15, E16, E17: the far search, the picture at large, the smoothed field (2026-10-20)

Lab only (`tools/flowlab/field_lab.py`, each idea applied to both flow fields and scored by the frame drawn between), on three real segments and on a made-up scene with the hard cases (`tools/flowlab/make_synth_clip.py`: a textured wall panning (14, 3) px a frame; a block of fine upright lines at the wall's own motion, where 14 px is two periods of the lines so the lines alone cannot show it; a plain gradient block moving with the wall; a 5 px pole moving 31 px a frame; every case moves with the wall, so there is no fixed window edge). The synthetic scene is where the estimator's flaws are visible: on it the wall is 97 % right, the pole about 35-70 % right (median 31, correct), and **the repeating block and the plain block come out as no motion at all** (the lines match as well at zero as at the truth; the plain gradient matches anywhere).

| idea | real clips (gain over the blend, direct draw, base +2.36 / +2.03 / +2.56) | made-up scene |
|---|---|---|
| E17 smooth the field (outliers, then a similarity-weighted 5 x 5 mean) | +0.00 / +0.02 / +0.05 dB | repeating block +0.2 dB, nothing else |
| E15 far search (every second pixel within 24, for pixels that show something and are badly matched) | not run (changed 0.1 % of the pixels on the made-up scene) | no effect on the pole (the estimator already tracks it) |
| E16 the picture at large wins where its vector carries a pixel about as well as the pixel's own | -0.07 / +0.07 / +0.10 dB, but it changes 50-65 % of the pixels | **the repeating block: +4.0 dB (error 182 down to 10), the wall +1.1 dB; but the pole -2.4 dB and the plain block -1.9 dB** |

- **E17 and E15: nothing worth building** on this evidence. The estimator's own median and its pixel pass already smooth the field, and it finds the fast thin pole.
- **E16 is the one that matters, and it is not finished**: it removes the big failure (a repeating texture or a fence read as no motion, which is what E2's "blocky cells on periodic textures" looks like) and a robust version of it is worth having, but the simple rule is not safe: on real footage "ambiguous" is common (soft pictures match about as well one or two pixels along), so it takes the large vector for half the pixels, and it loses on a pole and on a plain block because the picture at large is a mean (about 0.4 px off) and not the true vector of that block. A safe version needs (1) a robust central vector per tile (a mode, not a mean), (2) a gate that really means "repeating" (a self-similarity test at the *period* found in the patch, not at fixed offsets), and (3) the way-back check as a second opinion. Not built in; the lab, the scene and the numbers are the starting point.
- **What would settle it**: a real clip with a repeating texture or a fence moving with the camera (record one in WoW: a fence, a tower face, a stair railing), scored as above. Board E16 stays open with this note; E15 and E17 are closed as no gain.

### E16 on the GPU: the safe version built (2026-10-20)

- `src/engine/flow_fix.{h,cpp}` (nr::FlowFix): brightness at cell size, a per-tile mode of the trusted moving vectors, a per-cell decision (the tile's vector replaces the cell's only where the cell is plain or repeating, its own match is good, the picture's vector carries it about as well, and it is not faster than the picture), then a full-size apply into a texture of its own. The estimate's texture is untouched.
- Scores (nr_fgeval): the made-up scene whole +12.59 dB against +9.94 without it (the CPU lab version +13.68); real recordings 25.93 -> 25.93 dB and 38.60 -> 38.61 dB, and the real crops within +0.01. Safe on real footage, strong on the repeating-texture case.
- Cost at 2560x1440: 8.1 ms a pair at first; with 4 x 4 cells from 1920 wide, every fourth pixel voting and two repeat offsets instead of three, **3.8 ms median** (best 1.9, worst 6.1) with the same scores.
- In EchoFG's engine (`FgEngine`, test build): on by default, `NR_FLOW_FIX=0` turns it off for comparison. The presenter battery passes.
- Still wanted: a real clip with a fence, railing or tower face to see it on real footage; the cost could go lower (the tile mode is the heavy pass).

### E16 on five real clips: a building with a fence, and a power line (2026-10-20)

- Five Fallout: New Vegas recordings (1920x1080, ~700 frames each; `FalloutNV_2026-10-20_06-51-27` and `06-53-28` face a wooden building and a fence, `06-53-55`, `06-54-13`, `06-54-37` look down a road at power lines). Scored in 6 windows of 40 frames each, flow fix off against on.
- First version: neutral (within 0.05 dB) in 22 of 24 windows, but two losses after a hard turn when the scene was nearly still (-0.95 and -0.33 dB; frames 629-635 of `06-53-28` lost 2-8 dB): the tile's vector came from the few moving pixels (a character), not from the picture.
- Fix: a tile's vector is used only when it agrees with at least a quarter of all the trusted pixels in the window, moving or not (`gAll`). The losses are gone (36.89 against 36.90, 38.81 against 38.81) and the made-up scene still gains (the repeating block -0.76 -> +9.27 dB, whole +5.28 -> +5.91).
- Cost at 1920x1080: about 1 ms a pair (median 0.2-1.4 ms).
- What the clips do not show: a gain on the fence or the power line themselves. Whole-frame scores hide a small region; a crop score or a look at the frames is still to do.
