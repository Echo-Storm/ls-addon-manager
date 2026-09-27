"""Compares two nr_fgeval runs over the same recording, frame by frame.

    python tools/fgeval_compare.py <before.txt> <after.txt> [top=N]

Each file is nr_fgeval's printed output. Prints both averages of the generator's frames, how many frames got better or worse, and the
frames that changed most (with the motion the after run measured, when it printed it).
"""
import re
import sys

LINE = re.compile(r"frame\s+(\d+)\s+(\w+)\s+([\d.]+) dB .*?blend\s+([\d.]+) dB(?:\s+motion\s+(\d+) px)?")


def read(path):
    frames = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if m:
                frames[int(m.group(1))] = (float(m.group(3)), float(m.group(4)), int(m.group(5)) if m.group(5) else None)
    return frames


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    top = next((int(a[4:]) for a in sys.argv[3:] if a.startswith("top=")), 10)
    a, b = read(sys.argv[1]), read(sys.argv[2])
    common = sorted(set(a) & set(b))
    if not common:
        print("no frames in common")
        return 1
    # the averages leave out frames that did not move (99 dB), which would hide everything else
    moving = [k for k in common if a[k][0] < 60 and b[k][0] < 60]
    avg = lambda d, i: sum(d[k][i] for k in moving) / max(len(moving), 1)
    print(f"{len(moving)} moving frames of {len(common)}: before {avg(a, 0):.2f} dB, after {avg(b, 0):.2f} dB, blend {avg(a, 1):.2f} dB")
    better = sum(1 for k in moving if b[k][0] > a[k][0] + 0.1)
    worse = sum(1 for k in moving if b[k][0] < a[k][0] - 0.1)
    below = lambda d: sum(1 for k in moving if d[k][0] < d[k][1])
    print(f"better in {better}, worse in {worse}; below the blend: before {below(a)}, after {below(b)}")
    worst = sorted(moving, key=lambda k: a[k][0])[:top]
    print(f"the {top} worst before:")
    for k in worst:
        motion = f", motion {b[k][2]} px" if b[k][2] is not None else ""
        print(f"  frame {k:4d}: {a[k][0]:5.2f} -> {b[k][0]:5.2f} dB (blend {a[k][1]:5.2f}){motion}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
