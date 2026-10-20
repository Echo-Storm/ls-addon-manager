# recordings

Local test recordings (`.lsrec`, packed `.lsrec.zst` / `.lsrec.mkv`). Not in git (large); back this folder up yourself.
`python tools/dev.py` reads it through `.dev/config.json` (`recordings`). The addon's recorder saves to Videos\Lossless Scaling:
move the clips here after recording.

Suggested set for model A/B: calm detail, fast pan, dark/high-contrast with bright UI, one HDR clip.

## Clips of 2026-10-19 (WoW, 2560x1440, Lossless Scaling frame generation off, no addon on: the game's frames as captured; Compact, 8-bit SDR picture)

The baseline for the model comparisons (handoff board N3/N4). Labelled from a frame of each and the motion measured with `nr_lsrec flicker` (the mean change from one frame to the next, in levels of 255).

| File (WowB_2026-10-19_...) | Length | Motion | What it shows |
|---|---|---|---|
| 23-16-14 | 0.7 s | 0.07 | the recorder had just started: too short to use |
| 23-16-25 | 11.5 s | 3.3 | Valley of Strength street at a walk, quest tracker and NPCs in the distance |
| 23-16-37 | 15 s | 6.2 | Valley of Strength, a building with lit windows, NPCs and a large floating name |
| 23-16-58 | 15 s | 6.1 | Valley of Strength, characters close by, camera movement (the biggest steps: 30) |
| 23-17-18 | 15 s | 2.3 | a canyon wall and a large cactus, nearly still: fine detail (the calmest clip) |
| 23-17-55 | 15 s | 3.1 | an Orgrimmar interior, a dark room with one NPC |
| 23-18-17 | 15 s | 5.6 | Durotar at sunset: a red sky with dark silhouettes (high contrast) |
