"""order_lab.py: Neural Rendering before or after the upscaler (issue #23). Scores the two orders against the look Neural Rendering gives the full-size frame.

The question: the upscaler addons take the place of Lossless Scaling's NIS pass, and Neural Rendering's delta is added to the picture the screen shows (after the upscale, at the
screen's size). Would it be better, or cheaper, to add the delta to the game's own (smaller) frame and give the upscaler that picture?

For a recording at the "screen" size (say 2560x1440) and a game frame 1.5x smaller (1706x960), per frame:
  T  the target: Neural Rendering run on the full-size frame (the look it gives when it sees every pixel)
  U  the upscaler's picture of the shrunk frame, no Neural Rendering
  A  today's order: U plus the delta Neural Rendering made on the shrunk frame, stretched to the screen's size (the compose's own arithmetic)
  B  the proposed order: the shrunk frame plus its delta (at the game's size), then upscaled
Higher is better for A and B against T: how close each is to the look the model gives when it sees the full-size picture. Also the steadiness: the frame-to-frame change of each
against T's own change (a temporal upscaler fed a picture that changes might flicker or trail).

Make the files with the evaluators (tmp folder FOLDER; REC the recording, N frames from FIRST, BACKEND fsr|dlss):
  nr_nreval REC o_T first=FIRST count=N scale=100 show=0 rawout=FOLDER/T                     (T: pic_NNNNN.rgba)
  nr_nreval REC o_S first=FIRST count=N scale=100 shrink=150 show=0 rawout=FOLDER/S          (S: low (the shrunk frame), pic (frame plus delta), delta .f16)
  nr_sreval REC o_U first=FIRST count=N shrink=150 backend=BACKEND show=0 rawout=FOLDER/U    (U: up_NNNNN.rgba)
  (copy FOLDER/S/pic_NNNNN.rgba to FOLDER/Bin/low_NNNNN.rgba)
  nr_sreval REC o_B first=FIRST count=N shrink=150 backend=BACKEND show=0 inject=FOLDER/Bin rawout=FOLDER/B
then:  python tools/order_lab.py FOLDER U B [skip=8]      (U and B are the folders of the two upscaler runs)
"""
import os
import sys

import numpy as np

MAX_DELTA = 0.5      # the compose's limit on one pixel's change (nr_nreval maxdelta=50)
DARK_GUARD = 0.04    # compose11's dark guard (nr_nreval darkguard=4)


def read_rgba(path, w, h):
    return np.fromfile(path, dtype=np.uint8).reshape(h, w, 4)[:, :, :3]


def read_delta(path, w, h):
    return np.fromfile(path, dtype=np.float16).reshape(h, w, 4)[:, :, :3].astype(np.float32)


def stretch(delta, W, H):
    """The delta (h x w x 3) at the picture's size, bilinear, pixel centres aligned (nr_nreval's SampleDelta)."""
    dh, dw = delta.shape[:2]
    fx = np.clip((np.arange(W) + 0.5) * dw / W - 0.5, 0, dw - 1)
    fy = np.clip((np.arange(H) + 0.5) * dh / H - 0.5, 0, dh - 1)
    x0 = fx.astype(int); x1 = np.minimum(x0 + 1, dw - 1); ax = (fx - x0)[None, :, None]
    y0 = fy.astype(int); y1 = np.minimum(y0 + 1, dh - 1); ay = (fy - y0)[:, None, None]
    top = delta[y0][:, x0] * (1 - ax) + delta[y0][:, x1] * ax
    bot = delta[y1][:, x0] * (1 - ax) + delta[y1][:, x1] * ax
    return top * (1 - ay) + bot * ay


def compose(frame8, delta, intensity=1.0):
    """The frame plus the delta, as the addon's compose adds it (limited, the dark guard)."""
    H, W = frame8.shape[:2]
    d = np.clip(stretch(delta, W, H) * intensity, -MAX_DELTA, MAX_DELTA)
    f = frame8.astype(np.float32) / 255.0
    top = f.max(axis=2, keepdims=True)
    t = np.clip(top / max(DARK_GUARD, 1e-4), 0, 1)
    fade = t * t * (3 - 2 * t)
    neg = np.minimum(d, 0)
    d = neg + (d - neg) * fade
    return np.clip((f + d) * 255.0 + 0.5, 0, 255).astype(np.uint8)


def psnr(a, b):
    mse = np.mean((a.astype(np.float32) - b.astype(np.float32)) ** 2)
    return 99.0 if mse <= 1e-12 else 10 * np.log10(255.0 ** 2 / mse)


def luma(a):
    a = a.astype(np.float32)
    return 0.2126 * a[:, :, 0] + 0.7152 * a[:, :, 1] + 0.0722 * a[:, :, 2]


def box_mean(x, k=8):
    c = np.cumsum(np.cumsum(x, axis=0), axis=1)
    c = np.pad(c, ((1, 0), (1, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def ssim(a, b):
    """Structural similarity on luma, 8x8 boxes (1 = the same)."""
    x, y = luma(a), luma(b)
    c1, c2 = 6.5025, 58.5225
    mx, my = box_mean(x), box_mean(y)
    vx = box_mean(x * x) - mx * mx; vy = box_mean(y * y) - my * my; cxy = box_mean(x * y) - mx * my
    s = ((2 * mx * my + c1) * (2 * cxy + c2)) / ((mx * mx + my * my + c1) * (vx + vy + c2))
    return float(s.mean())


def steady(x, xp, t, tp):
    """The frame-to-frame change of x against the target's own change (dB; higher is steadier), luma."""
    d = (luma(x) - luma(xp)) - (luma(t) - luma(tp))
    mse = np.mean(d * d)
    return 99.0 if mse <= 1e-12 else 10 * np.log10(255.0 ** 2 / mse)


def main():
    if len(sys.argv) < 4:
        print(__doc__); return 2
    root, up_dir, b_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    skip = 8
    for a in sys.argv[4:]:
        if a.startswith('skip='): skip = int(a[5:])
    lw, lh, dw, dh = map(int, open(os.path.join(root, 'S', 'dims.txt')).read().split())
    Wt, Ht, _, _ = map(int, open(os.path.join(root, 'T', 'dims.txt')).read().split())
    frames = sorted(int(f[4:9]) for f in os.listdir(os.path.join(root, 'T')) if f.startswith('pic_'))
    rows = []
    prev = None
    for k, n in enumerate(frames):
        T = read_rgba(os.path.join(root, 'T', 'pic_%05d.rgba' % n), Wt, Ht)
        U = read_rgba(os.path.join(root, up_dir, 'up_%05d.rgba' % n), Wt, Ht)
        B = read_rgba(os.path.join(root, b_dir, 'up_%05d.rgba' % n), Wt, Ht)
        D = read_delta(os.path.join(root, 'S', 'delta_%05d.f16' % n), dw, dh)
        A = compose(U, D)
        if prev is not None and k >= skip:
            pT, pU, pA, pB = prev
            rows.append((psnr(U, T), psnr(A, T), psnr(B, T), ssim(U, T), ssim(A, T), ssim(B, T), steady(U, pU, T, pT), steady(A, pA, T, pT), steady(B, pB, T, pT)))
        prev = (T, U, A, B)
    r = np.array(rows)
    m = r.mean(axis=0)
    print('%d frames (after the first %d), %dx%d screen, game frame %dx%d, delta %dx%d' % (len(r), skip, Wt, Ht, lw, lh, dw, dh))
    print('                          no NR (U)    NR after the upscale (A, today)    NR before the upscale (B)')
    print('PSNR to the NR target     %8.2f     %8.2f                        %8.2f' % (m[0], m[1], m[2]))
    print('SSIM to the NR target     %8.4f     %8.4f                        %8.4f' % (m[3], m[4], m[5]))
    print('steadiness (dB)           %8.2f     %8.2f                        %8.2f' % (m[6], m[7], m[8]))
    wins = int((r[:, 2] > r[:, 1]).sum())
    print('B closer to the target than A in %d of %d frames (PSNR); A %+.2f dB against U, B %+.2f dB against U' % (wins, len(r), m[1] - m[0], m[2] - m[0]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
