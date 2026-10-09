"""A lab for frame interpolation ideas, CPU only (numpy), on small versions of real frames.

    python tools/interp_lab.py <folder of exported frames *.bmp> [--step 5] [--triples 24] [--range 10] [--block 9]

Takes three frames in a row (a, b, c) from `nr_lsrec export`, rebuilds b from a and c, and scores it against the real b (PSNR, higher is better): the same test as `nr_fgeval`,
at a fraction of the size, so that ideas can be tried in minutes. Every method here is written from public ideas (block matching, bilateral / symmetric motion estimation for
motion-compensated interpolation, a safe fallback where the match is poor); none comes from Lossless Scaling's or anyone else's code.

Methods
  blend      the two frames averaged (the floor)
  fwd        motion measured from c back to a at the blocks of c, the frame between made by moving a and c half way along it (the usual way; leaves holes and doubles where things move)
  bilat      motion measured *at the middle frame*: for each pixel the shift s that makes a(x - s) and c(x + s) agree (a symmetric search), so there are no holes
  bilat+fb   bilat, but where the match is poor (a cost over a threshold) the plain blend is used instead
  bilat+sel  bilat, but where the match is poor the better of a(x), c(x) and the blend is chosen by agreement with the matched neighbours' colour (a cheap occlusion rule)
"""
import argparse
import glob
import os
import sys

import numpy as np
from PIL import Image


def load(path, scale):
    im = Image.open(path).convert('RGB')
    w, h = im.size
    im = im.resize((w // scale, h // scale), Image.BOX)
    return np.asarray(im, np.float32)


def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    return 99.0 if mse < 1e-9 else 10.0 * np.log10(255.0 * 255.0 / mse)


def box(a, k):
    """mean over a k x k window (k odd), edges replicated; a is 2-D"""
    r = k // 2
    p = np.pad(a, r, mode='edge')
    c = np.cumsum(np.cumsum(p, axis=0), axis=1)
    c = np.pad(c, ((1, 0), (1, 0)))
    s = c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]
    return s / float(k * k)


def shift(img, dx, dy):
    """img sampled at (x + dx, y + dy), edges clamped; works for 2-D and 3-D"""
    h, w = img.shape[:2]
    ys = np.clip(np.arange(h) + dy, 0, h - 1)
    xs = np.clip(np.arange(w) + dx, 0, w - 1)
    return img[ys][:, xs]


def gather(img, dxmap, dymap):
    h, w = img.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w]
    return img[np.clip(yy + dymap, 0, h - 1), np.clip(xx + dxmap, 0, w - 1)]


def bilateral_motion(a, c, rng, block, lam, margin=9.0, verify=6.0):
    """for each pixel the integer shift s (dx, dy) with a(x - s) ~ c(x + s): the best of the candidates by block cost + a small penalty for motion; also the cost found"""
    h, w = a.shape[:2]
    best = np.full((h, w), 1e9, np.float32); sx = np.zeros((h, w), np.int32); sy = np.zeros((h, w), np.int32)
    for dy in range(-rng, rng + 1):
        for dx in range(-rng, rng + 1):
            d = np.abs(shift(a, -dx, -dy) - shift(c, dx, dy)).sum(axis=2)
            cost = box(d, block) + lam * (abs(dx) + abs(dy))
            better = cost < best
            best = np.where(better, cost, best); sx = np.where(better, dx, sx); sy = np.where(better, dy, sy)
    # motion is accepted only where it beats staying put by a margin (noise and texture otherwise pick small shifts in a still picture)
    cost0 = box(np.abs(a - c).sum(axis=2), block)
    still = best + margin > cost0
    sx = np.where(still, 0, sx); sy = np.where(still, 0, sy); best = np.where(still, cost0, best)
    # per pixel: the shifted pair must agree better than the unshifted pair here, or the pixel stays put (a block's best shift is wrong for the odd pixel at an edge)
    d_s = np.abs(gather(a, -sx, -sy) - gather(c, sx, sy)).sum(axis=2)
    d_0 = np.abs(a - c).sum(axis=2)
    keep = d_s + verify < d_0
    return np.where(keep, sx, 0), np.where(keep, sy, 0), best


def down2(img):
    h, w = img.shape[0] // 2 * 2, img.shape[1] // 2 * 2
    i = img[:h, :w]
    return 0.25 * (i[0::2, 0::2] + i[1::2, 0::2] + i[0::2, 1::2] + i[1::2, 1::2])


def up2(m, shape):
    """a shift map one level coarser, doubled and repeated to this level's size"""
    u = np.repeat(np.repeat(m, 2, axis=0), 2, axis=1) * 2
    h, w = shape
    if u.shape[0] < h: u = np.pad(u, ((0, h - u.shape[0]), (0, 0)), mode='edge')
    if u.shape[1] < w: u = np.pad(u, ((0, 0), (0, w - u.shape[1])), mode='edge')
    return u[:h, :w]


def bilateral_motion_h(a, c, levels=3, rng0=5, refine=2, block=7, lam=0.5, margin=9.0, verify=6.0):
    """coarse to fine: at the coarsest level a full search of +-rng0, then at each finer level +-refine around the doubled estimate, per pixel; the shift is the one that makes
    a(x - s) and c(x + s) agree"""
    pa, pc = [a], [c]
    for _ in range(levels - 1):
        pa.append(down2(pa[-1])); pc.append(down2(pc[-1]))
    sx = sy = None
    for lv in range(levels - 1, -1, -1):
        A, C = pa[lv], pc[lv]
        h, w = A.shape[:2]
        if sx is None:
            cx = np.zeros((h, w), np.int32); cy = np.zeros((h, w), np.int32); r = rng0
        else:
            cx = up2(sx, (h, w)).astype(np.int32); cy = up2(sy, (h, w)).astype(np.int32); r = refine
        best = np.full((h, w), 1e9, np.float32); bx = cx.copy(); by = cy.copy()
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                tx, ty = cx + dx, cy + dy
                d = np.abs(gather(A, -tx, -ty) - gather(C, tx, ty)).sum(axis=2)
                cost = box(d, block) + lam * (np.abs(tx) + np.abs(ty)) * (0.5 ** lv)
                better = cost < best
                best = np.where(better, cost, best); bx = np.where(better, tx, bx); by = np.where(better, ty, by)
        sx, sy = bx, by
        if lv == 0:
            cost0 = box(np.abs(A - C).sum(axis=2), block)
            still = best + margin > cost0
            sx = np.where(still, 0, sx); sy = np.where(still, 0, sy); best = np.where(still, cost0, best)
            d_s = np.abs(gather(A, -sx, -sy) - gather(C, sx, sy)).sum(axis=2)
            keep = d_s + verify < np.abs(A - C).sum(axis=2)
            sx = np.where(keep, sx, 0); sy = np.where(keep, sy, 0)
            return sx, sy, best
    return sx, sy, best


def forward_motion(a, c, rng, block, lam, margin=9.0):
    """for each pixel of c the integer motion m (dx, dy) with c(x) ~ a(x + m): the motion of the picture between the frames is -m"""
    h, w = a.shape[:2]
    best = np.full((h, w), 1e9, np.float32); mx = np.zeros((h, w), np.int32); my = np.zeros((h, w), np.int32)
    for dy in range(-2 * rng, 2 * rng + 1, 2):
        for dx in range(-2 * rng, 2 * rng + 1, 2):
            d = np.abs(c - shift(a, dx, dy)).sum(axis=2)
            cost = box(d, block) + lam * (abs(dx) + abs(dy))
            better = cost < best
            best = np.where(better, cost, best); mx = np.where(better, dx, mx); my = np.where(better, dy, my)
    cost0 = box(np.abs(c - a).sum(axis=2), block)
    still = best + margin > cost0
    return np.where(still, 0, mx), np.where(still, 0, my), np.where(still, cost0, best)


def run(a, c, b, rng, block):
    out = {}
    out['blend'] = 0.5 * (a + c)
    # fwd: move a half way toward c's side and c half way back, along the motion measured at c's blocks
    mx, my, _ = forward_motion(a, c, rng, block, 0.5)
    wa = gather(a, mx // 2, my // 2)            # a at x + m/2
    wc = gather(c, -(mx // 2), -(my // 2))      # c at x - m/2 (c's picture came from a's at the motion m, so half way back)
    out['fwd'] = 0.5 * (wa + wc)
    # bilat: symmetric search at the middle frame
    sx, sy, cost = bilateral_motion(a, c, rng, block, 0.5)
    ma = gather(a, -sx, -sy); mc = gather(c, sx, sy)
    bil = 0.5 * (ma + mc)
    out['bilat'] = bil
    thr = 3 * 18.0                              # a per-pixel colour difference of about 18 levels over three channels counts as poor
    poor = (cost / 1.0) > thr
    out['bilat+fb'] = np.where(poor[..., None], out['blend'], bil)
    # sel: where poor, take whichever of a(x), c(x), blend is closest to the matched neighbourhood's mean colour
    nb = np.stack([box(bil[..., k], 2 * block + 1) for k in range(3)], axis=2)
    cands = [a, c, out['blend']]
    errs = [np.abs(cnd - nb).sum(axis=2) for cnd in cands]
    pick = np.argmin(np.stack(errs, axis=0), axis=0)
    chosen = np.choose(pick[..., None], cands)
    out['bilat+sel'] = np.where(poor[..., None], chosen, bil)
    # bilatH: the symmetric search coarse to fine (reaches far further for the same work), and with the same fallbacks
    hx, hy, hcost = bilateral_motion_h(a, c)
    hil = 0.5 * (gather(a, -hx, -hy) + gather(c, hx, hy))
    hpoor = hcost > thr
    out['bilatH'] = hil
    out['bilatH+fb'] = np.where(hpoor[..., None], out['blend'], hil)
    nb2 = np.stack([box(hil[..., k], 2 * block + 1) for k in range(3)], axis=2)
    cands2 = [a, c, out['blend']]
    pick2 = np.argmin(np.stack([np.abs(cnd - nb2).sum(axis=2) for cnd in cands2], axis=0), axis=0)
    out['bilatH+sel'] = np.where(hpoor[..., None], np.choose(pick2[..., None], cands2), hil)
    return {k: psnr(np.clip(v, 0, 255), b) for k, v in out.items()}, float(poor.mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('folder')
    ap.add_argument('--scale', type=int, default=5, help='the frames are made this many times smaller')
    ap.add_argument('--triples', type=int, default=24)
    ap.add_argument('--range', type=int, default=10, help='search range of the shift, in small pixels')
    ap.add_argument('--block', type=int, default=9)
    ap.add_argument('--stride', type=int, default=3, help='every stride-th triple')
    args = ap.parse_args()
    files = sorted(glob.glob(os.path.join(args.folder, '*.bmp')))
    if len(files) < 3:
        sys.exit('need at least three .bmp frames in the folder')
    sums = {}; polls = []; n = 0
    for i in range(0, len(files) - 2, args.stride):
        a, b, c = (load(files[i + k], args.scale) for k in range(3))
        scores, poor = run(a, c, b, args.range, args.block)
        for k, v in scores.items():
            sums.setdefault(k, []).append(v)
        polls.append(poor); n += 1
        if n >= args.triples:
            break
    print('%d triples from %s (frames %dx%d, search +-%d, block %d); poor-match share %.1f%%' % (n, args.folder, a.shape[1], a.shape[0], args.range, args.block, 100 * np.mean(polls)))
    for k in ('blend', 'fwd', 'bilat', 'bilat+fb', 'bilat+sel', 'bilatH', 'bilatH+fb', 'bilatH+sel'):
        v = np.array(sums[k])
        print('  %-10s mean %.2f dB   (min %.2f, max %.2f)' % (k, v.mean(), v.min(), v.max()))


if __name__ == '__main__':
    main()
