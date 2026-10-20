"""hud_lab.py RUN_FOLDER [w=1280 h=720] [pairs=40]: how a frame drawn between two real frames treats a HUD that stays put over a picture that moves.

RUN_FOLDER is what flowlab.exe wrote for a clip with a HUD over moving ground (docs/mfg-super-motion-engine-notes.md, step 9: marks), the whole frame at half size
(`down=2`): frames.rgba (even frames real, odd ones the truth between), ours.f32 (the flow at the later frame back to the earlier) and ours_rev.f32 (the other way).

The HUD is found automatically, from the truth: a pixel that does not change by more than a few levels across the whole run while the picture around it moves is a HUD
pixel (a mask for scoring only; the methods below do not see it). Each method draws the frame half way and is scored on the HUD pixels and on the rest separately:
  blend     the two real frames averaged
  direct    the motion at the pixel from both flow fields, drawn from both real frames (nothing carried; tools/flowlab/synth_lab.py)
  marks     direct, plus per-pixel marks learnt from past frames only: a pixel that was alike in both real frames of each of the last N pairs while the flow around it
            was moving is a mark (count up by two, down by one when it goes with its place); a mark is held as the earlier frame has it where the later frame has it the same
"""
import sys
import numpy as np
from synth_lab import sample, luma, box, psnr, mismatch, warp_pair


def main():
    folder = sys.argv[1]
    args = {k: v for k, v in (s.split('=') for s in sys.argv[2:])}
    w = int(args.get('w', 1280)); h = int(args.get('h', 720))
    frames = np.memmap(folder + '/frames.rgba', np.uint8, 'r').reshape(-1, h, w, 4)
    Fc_all = np.memmap(folder + '/ours.f32', np.float32, 'r').reshape(-1, h, w, 2)
    Fa_all = np.memmap(folder + '/ours_rev.f32', np.float32, 'r').reshape(-1, h, w, 2)
    pairs = min(len(Fc_all), len(Fa_all), int(args.get('pairs', 99)))
    reals = [luma(frames[2 * i, :, :, :3].astype(np.float32)) for i in range(len(frames) // 2)]
    # the HUD, from the whole run: the pixels that never change
    stack = np.stack(reals)
    still = (stack.max(0) - stack.min(0)) < float(args.get('still', 10))
    print('HUD pixels (never change across the run): %.1f %% of the picture' % (100 * still.mean()))
    gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    border = np.zeros((h, w), bool); border[24:h - 24, 24:w - 24] = True
    hud = still & border; rest = ~still & border
    count = np.zeros((h, w), np.int32)       # the marks' counts, learnt from past pairs
    N = int(args.get('mark', 4))
    names = ['blend', 'direct', 'marks', 'marks2', 'marks3', 'oracle']
    count2 = np.zeros((h, w), np.int32)
    res = {n: [] for n in names}
    for p in range(pairs):
        r = p + 1
        A = frames[2 * (r - 1), :, :, :3].astype(np.float32); C = frames[2 * r, :, :, :3].astype(np.float32); T = frames[2 * r - 1, :, :, :3].astype(np.float32)
        Fc = Fc_all[p].astype(np.float32); Fa = Fa_all[p].astype(np.float32)
        f = Fa; b = -Fc
        agree = np.hypot(*(f + (-b)).transpose(2, 0, 1)) <= 1.0 + 0.2 * np.hypot(f[..., 0], f[..., 1])
        mf, mb = mismatch(A, C, f, gx, gy), mismatch(A, C, b, gx, gy)
        v = np.where(agree[..., None], 0.5 * (f + b), np.where((mf <= mb)[..., None], f, b))
        a, c = warp_pair(A, C, v, gx, gy)
        direct = 0.5 * (a + c)
        # marks, learnt only from what is known by now (this pair's own two real frames and earlier): alike in both frames, while its place moves
        alike = np.abs(luma(A) - luma(C)) < 4.0
        moving = box(np.hypot(v[..., 0], v[..., 1]), 7) > 1.0
        count = np.where(alike & moving, np.minimum(count + 2, 12), np.where(moving, np.maximum(count - 1, 0), count))
        mark = (count >= N) & alike
        marked = np.where(mark[..., None], 0.5 * (A + C), direct)
        # marks2: the test of their recipe: drawn by the way the picture at large moves here, would the pixel come out clearly another colour (in both real frames)?
        Al, Cl = luma(A), luma(C)
        notalike = ~alike
        wgt = box(notalike.astype(np.float32), 41)
        large = np.stack([box(v[..., 0] * notalike, 41), box(v[..., 1] * notalike, 41)], -1) / np.maximum(wgt, 1e-3)[..., None]
        large = np.where((wgt > 0.15)[..., None], large, 0)
        big = np.hypot(large[..., 0], large[..., 1]) > 1.5
        diffA = np.abs(luma(sample(A, gx - 0.5 * large[..., 0], gy - 0.5 * large[..., 1])) - Al)
        diffC = np.abs(luma(sample(C, gx + 0.5 * large[..., 0], gy + 0.5 * large[..., 1])) - Cl)
        changed = (diffA > 10) | (diffC > 10)
        count2 = np.where(alike & big & changed, np.minimum(count2 + 2, 12), np.where(big, np.maximum(count2 - 1, 0), count2))
        mark2 = (count2 >= int(args.get('mark2', 2))) & alike
        marked2 = np.where(mark2[..., None], 0.5 * (A + C), direct)
        # marks3: marks2's counts, held wherever the count stands (not only where alike this pair) if the later frame has it the same within a looser tolerance,
        # and the mark grown by one pixel (the anti-aliased edge of a letter is never quite alike)
        hold = (count2 >= int(args.get('mark2', 2))) & (np.abs(Al - Cl) < float(args.get('tol', 14)))
        hold = (box(hold.astype(np.float32), 3) > 0.01) & (np.abs(Al - Cl) < float(args.get('tol', 14)) * 2)
        marked3 = np.where(hold[..., None], 0.5 * (A + C), direct)
        orc = np.where(still[..., None], 0.5 * (A + C), direct)
        out = {'blend': 0.5 * (A + C), 'direct': direct, 'marks': marked, 'marks2': marked2, 'marks3': marked3, 'oracle': orc}
        for n in names:
            e = (out[n] - T) ** 2
            e = e.mean(-1)
            res[n].append((10 * np.log10(255 * 255 / max(e[hud].mean(), 1e-6)), 10 * np.log10(255 * 255 / max(e[rest].mean(), 1e-6))))
    print('%d pairs: PSNR dB on the HUD pixels / on the rest' % pairs)
    for n in names:
        a = np.array(res[n])
        print('%-7s HUD %6.2f dB   rest %6.2f dB' % (n, a[:, 0].mean(), a[:, 1].mean()))


if __name__ == '__main__':
    main()
