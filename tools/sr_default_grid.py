#!/usr/bin/env python
"""Runs nr_sreval over a grid of the upscalers' picture settings on some recordings and prints one table (used to choose defaults).

  python tools/sr_default_grid.py <shrink> <backend> <recording.lsrec> [more recordings] [--first N] [--count N]

Each cell is "upscaled dB / steady dB" (upscaled: against the game's full-size frame, only meaningful when shrink > 100; steady: frame to frame),
averaged over the recordings. sharpen is the Sharpening slider, rest is "Sharpness at rest", steady is "Steady sharpening".
"""
import itertools, re, subprocess, sys, os, tempfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
exe = os.path.join(root, "addons", "DLSS5NR01", "build", "Release", "nr_sreval.exe")

def main():
    args = sys.argv[1:]
    first, count = 40, 30
    if "--first" in args: i = args.index("--first"); first = int(args[i + 1]); del args[i:i + 2]
    if "--count" in args: i = args.index("--count"); count = int(args[i + 1]); del args[i:i + 2]
    shrink, backend, clips = args[0], args[1], args[2:]
    grid = list(itertools.product([30, 50, 70], [0, 30, 60], [0, 60]))   # sharpen %, rest %, steady %
    out = tempfile.mkdtemp(prefix="srgrid_")
    print(f"shrink {shrink} backend {backend} clips {len(clips)} frames {first}+{count}")
    print("sharpen rest steady | upscaled dB / steady dB per clip, then the mean")
    for sharpen, rest, steady in grid:
        cells = []
        for clip in clips:
            cmd = [exe, clip, out, f"first={first}", f"count={count}", f"shrink={shrink}", f"backend={backend}", f"sharpen={sharpen}", f"restmix={rest}"]
            if steady: cmd.append(f"steady={steady}")
            text = subprocess.run(cmd, capture_output=True, text=True).stdout
            m = re.search(r"average over \d+ frames.*?upscaled ([\d.]+) dB \(coarse [\d.]+, steady ([\d.]+)\)", text)
            cells.append((float(m.group(1)), float(m.group(2))) if m else (float("nan"), float("nan")))
        mean = (sum(c[0] for c in cells) / len(cells), sum(c[1] for c in cells) / len(cells))
        print(f"{sharpen/100:>7.2f} {rest/100:>4.2f} {steady/100:>6.2f} | " + "  ".join(f"{a:.2f}/{b:.2f}" for a, b in cells) + f" | mean {mean[0]:.2f}/{mean[1]:.2f}", flush=True)

main()
