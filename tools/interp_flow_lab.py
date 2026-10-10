"""Synthesis ideas for a frame between two frames, tested on this project's own motion vectors (CPU only, numpy, frames made smaller).

    nr_fgeval <recording.lsrec> <folder> count=42 flowdump=10        (writes triple_NNNNN.bin: the frame before, the real frame between, this frame, the vectors)
    python tools/interp_flow_lab.py <folder> [--scale 4] [--only 3]

tools/interp_lab.py found that block matching is too weak a motion to judge synthesis by; here the motion is the estimator's (src/engine/flow_estimator.cpp), the
same that feeds FSR frame generation (30.4 dB against 26.1 for a blend on Silent Hill f), so a method's score isolates the question: given good vectors, what
makes the frame between? Every method is written from public ideas (motion compensated interpolation, splatting the motion to the time of the frame, occlusion
by match error); none comes from Lossless Scaling's or anyone else's code.

The vectors v at a pixel x of the frame c point to where that pixel was in the frame a: c(x) ~ a(x + v). At time t (0 = a, 1 = c) the same content is at x + (1 - t) v.

Methods (t = 0.5)
  blend    the two frames averaged (the floor)
  half     each output pixel q takes c(q - v(q)/2) and a(q + v(q)/2): the vectors read at the output pixel as if they were defined there (the usual cheap way)
  spl      the vectors first splatted to where their pixels are at time t (x + v/2, the best matching pixel wins a spot), so they are defined at the output pixel
  spl+st   spl, but where the frames agree without any motion (HUD, still background) their mean, not the moved one
  spl+occ  spl+st, and where the moved pair disagrees: a hole of the splat (covered: only a shows it) takes a, a pixel whose own match was poor (uncovered: only c shows it) takes c
  oracle   the best per pixel of (blend, a, c, moved pair, moved a, moved c) with the truth known: what a perfect choice rule would reach (not a method)
"""
import argparse
import glob
import os
import struct
import sys

import numpy as np


def load_triple(path, scale):
    with open(path, 'rb') as f:
        magic, w, h, tm = struct.unpack('<4I', f.read(16))
        if magic not in (0x31544C46, 0x32544C46):
            raise ValueError('not a flow triple: ' + path)
        n = w * h
        fr = [np.frombuffer(f.read(n * 4), np.uint8).reshape(h, w, 4)[..., :3].astype(np.float32) for _ in range(3)]
        mv = np.frombuffer(f.read(n * 4), np.float16).reshape(h, w, 2).astype(np.float32)
        mvb = np.frombuffer(f.read(n * 4), np.float16).reshape(h, w, 2).astype(np.float32) if magic == 0x32544C46 else None

    def small(img):
        hh, ww = h // scale * scale, w // scale * scale
        i = img[:hh, :ww]
        return i.reshape(hh // scale, scale, ww // scale, scale, -1).mean(axis=(1, 3))
    a, b, c = (small(x) for x in fr)
    v = small(mv) / scale          # vectors in the small frame's pixels
    vb = small(mvb) / scale if mvb is not None else None   # the other way: a pixel of a to where it is in c
    return a, b, c, v, tm / 1000.0 if tm else 0.5, vb


def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    return 99.0 if mse < 1e-9 else 10.0 * np.log10(255.0 * 255.0 / mse)


def box(a, k):
    r = k // 2
    p = np.pad(a, [(r, r), (r, r)] + [(0, 0)] * (a.ndim - 2), mode='edge')
    c = np.cumsum(np.cumsum(p, axis=0), axis=1)
    c = np.pad(c, [(1, 0), (1, 0)] + [(0, 0)] * (a.ndim - 2))
    s = c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]
    return s / float(k * k)


def bilin(img, xs, ys):
    """img sampled at float positions (xs, ys), edges clamped"""
    h, w = img.shape[:2]
    xs = np.clip(xs, 0, w - 1.001); ys = np.clip(ys, 0, h - 1.001)
    x0 = xs.astype(np.int32); y0 = ys.astype(np.int32)
    fx = (xs - x0)[..., None]; fy = (ys - y0)[..., None]
    return (img[y0, x0] * (1 - fx) + img[y0, x0 + 1] * fx) * (1 - fy) + (img[y0 + 1, x0] * (1 - fx) + img[y0 + 1, x0 + 1] * fx) * fy


def bicubic(img, xs, ys):
    """Catmull-Rom: sharper than bilinear (two bilinear taps averaged blur twice)"""
    h, w = img.shape[:2]
    xs = np.clip(xs, 0, w - 1.001); ys = np.clip(ys, 0, h - 1.001)
    x0 = np.floor(xs).astype(np.int32); y0 = np.floor(ys).astype(np.int32)
    fx = (xs - x0)[..., None]; fy = (ys - y0)[..., None]

    def wts(f):
        f2, f3 = f * f, f * f * f
        return (-0.5 * f3 + f2 - 0.5 * f, 1.5 * f3 - 2.5 * f2 + 1.0, -1.5 * f3 + 2.0 * f2 + 0.5 * f, 0.5 * f3 - 0.5 * f2)
    wx, wy = wts(fx), wts(fy)
    out = 0.0
    for j in range(4):
        yj = np.clip(y0 + j - 1, 0, h - 1)
        row = 0.0
        for i in range(4):
            xi = np.clip(x0 + i - 1, 0, w - 1)
            row = row + img[yj, xi] * wx[i]
        out = out + row * wy[j]
    return out


SAMPLER = bilin


def moved(img, v, k):
    """img sampled k * v away from each pixel"""
    h, w = img.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    return SAMPLER(img, xx + k * v[..., 0], yy + k * v[..., 1])


def match_error(a, c, v, blk=5):
    """per pixel: how unlike the pixel of c is the place of a its vector points to (colour levels summed over channels, 5x5 mean)"""
    return box(np.abs(c - moved(a, v, 1.0)).sum(axis=2), blk)


def splat(v, err, t=0.5):
    """the vectors put where their pixels are at time t (x + (1 - t) v); where several land the best match wins; returns the field and a mask of where one landed"""
    h, w = err.shape
    yy, xx = np.mgrid[0:h, 0:w]
    tx = np.clip(np.rint(xx + (1 - t) * v[..., 0]).astype(np.int32), 0, w - 1)
    ty = np.clip(np.rint(yy + (1 - t) * v[..., 1]).astype(np.int32), 0, h - 1)
    idx = (ty * w + tx).ravel()
    order = np.argsort(err.ravel())[::-1]        # worst first, so the best is written last and stays
    out = np.zeros((h * w, 2), np.float32); e = np.full(h * w, 1e9, np.float32); got = np.zeros(h * w, bool)
    out[idx[order]] = v.reshape(-1, 2)[order]
    e[idx[order]] = err.ravel()[order]
    got[idx] = True
    return out.reshape(h, w, 2), e.reshape(h, w), got.reshape(h, w)


def fill_holes(v_sp, got, v_fallback, iters=3):
    """a hole takes the mean of the vectors around it (a few rounds), then what is left the plain vectors"""
    v = v_sp.copy(); known = got.copy()
    for _ in range(iters):
        wsum = box(known.astype(np.float32), 3) * 9.0
        vsum = box(np.where(known[..., None], v, 0.0), 3) * 9.0
        fill = (~known) & (wsum > 0.5)
        v = np.where(fill[..., None], vsum / np.maximum(wsum, 1e-6)[..., None], v)
        known = known | fill
    return np.where(known[..., None], v, v_fallback)


args_margin = [10.0]


AGREE_BOX = [5]


def dirfill_pair(F, reached, a, c, t, base):
    """pixels not reached take the vector of the nearest reached pixel left, right, above or below (or none), whichever makes the moved pair agree best over 5x5"""
    cands = [F * 0.0]
    for axis, rev in ((1, False), (1, True), (0, False), (0, True)):
        Fa = np.flip(F, axis) if rev else F; Ra = np.flip(reached, axis) if rev else reached
        n = Ra.shape[axis]
        pos = np.arange(n).reshape((1, n) if axis == 1 else (n, 1))
        last = np.maximum.accumulate(np.where(Ra, pos, -1), axis=axis)
        src = np.take_along_axis(Fa, np.clip(last, 0, n - 1)[..., None].repeat(2, axis=2), axis=axis)
        src = np.where((last >= 0)[..., None], src, 0.0)
        cands.append(np.flip(src, axis) if rev else src)
    best_e = np.full(a.shape[:2], 1e9, np.float32); best_u = np.zeros_like(F)
    for u in cands:
        e = box(np.abs(moved(a, u, t) - moved(c, u, -(1 - t))).sum(axis=2), AGREE_BOX[0])
        better = e < best_e
        best_u = np.where(better[..., None], u, best_u); best_e = np.where(better, e, best_e)
    hole = (1 - t) * moved(a, best_u, t) + t * moved(c, best_u, -(1 - t))
    return np.where(reached[..., None], base, hole)


def methods(a, c, v, t=0.5, thr_pair=60.0, thr_poor=90.0, still_thr=24.0, vb=None, tau=1.0):
    out = {}
    out['blend'] = (1 - t) * a + t * c
    # half: vectors read at the output pixel
    half = (1 - t) * moved(a, v, t) + t * moved(c, v, -(1 - t))
    out['half'] = half
    # spl: vectors at the output pixel by splatting
    err = match_error(a, c, v)
    vs, e_at, got = splat(v, err, t)
    vs_raw = vs.copy()
    vs = fill_holes(vs, got, v)
    ca = moved(a, vs, t); cc = moved(c, vs, -(1 - t))
    spl = (1 - t) * ca + t * cc
    out['spl'] = spl
    # spl+st: where the frames agree unmoved (3x3 mean difference small) their mean is used
    d_still = box(np.abs(a - c).sum(axis=2), 3)
    still = d_still < still_thr
    spl_st = np.where(still[..., None], out['blend'], spl)
    out['spl+st'] = spl_st
    # spl+occ: where the moved pair disagrees pick by what the splat knows
    pair = box(np.abs(ca - cc).sum(axis=2), 3)
    bad = (pair > thr_pair) & ~still
    poor = e_at > thr_poor
    pick = np.where((~got)[..., None], ca, np.where(poor[..., None], cc, spl))
    out['spl+occ'] = np.where(bad[..., None], pick, spl_st)
    # spl+rel: the unmoved mean only where the unmoved pair agrees better than the moved pair does (a relative test, not a level)
    d_move = box(np.abs(ca - cc).sum(axis=2), 3)
    out['spl+rel'] = np.where((d_still < d_move)[..., None], out['blend'], spl)
    # cand: per output pixel the best of the neighbouring vectors (and none) by how well the moved pair agrees; a vector that wins is one that fits this pixel's surroundings
    best_v = vs.copy(); best_e = d_move.copy()
    h, w = a.shape[:2]
    for r in (2, 4):
        for dy in (-r, 0, r):
            for dx in (-r, 0, r):
                if dx == 0 and dy == 0:
                    continue
                u = np.roll(np.roll(vs, dy, axis=0), dx, axis=1)
                e = box(np.abs(moved(a, u, t) - moved(c, u, -(1 - t))).sum(axis=2), 3)
                better = e < best_e
                best_v = np.where(better[..., None], u, best_v); best_e = np.where(better, e, best_e)
    zero_e = d_still
    best_v = np.where((zero_e < best_e)[..., None], 0.0, best_v); best_e = np.minimum(best_e, zero_e)
    cand = (1 - t) * moved(a, best_v, t) + t * moved(c, best_v, -(1 - t))
    out['cand'] = cand
    # ref: the splatted vector nudged per pixel (+-1 pixel, half pixel steps) to where the moved pair agrees best over a 5x5 window, a small cost for nudging
    ref_v = vs.copy(); ref_e = box(np.abs(ca - cc).sum(axis=2), 5)
    for dy in (-1.0, -0.5, 0.0, 0.5, 1.0):
        for dx in (-1.0, -0.5, 0.0, 0.5, 1.0):
            if dx == 0 and dy == 0:
                continue
            u = vs + np.array([dx, dy], np.float32)
            e = box(np.abs(moved(a, u, t) - moved(c, u, -(1 - t))).sum(axis=2), 5) + 2.0 * (abs(dx) + abs(dy))
            better = e < ref_e
            ref_v = np.where(better[..., None], u, ref_v); ref_e = np.where(better, e, ref_e)
    out['ref'] = (1 - t) * moved(a, ref_v, t) + t * moved(c, ref_v, -(1 - t))
    if vb is not None:
        # bi: both flows; a vector is used only where the flow the other way agrees (forward-backward consistency), so what only one frame sees is not matched to the wrong place
        res_c = np.hypot(*np.moveaxis(v + moved(vb, v, 1.0), 2, 0))      # at a pixel of c: where it was in a, and back
        res_a = np.hypot(*np.moveaxis(vb + moved(v, vb, 1.0), 2, 0))     # at a pixel of a: where it is in c, and back
        ok_c = res_c < tau + 0.15 * np.hypot(v[..., 0], v[..., 1]); ok_a = res_a < tau + 0.15 * np.hypot(vb[..., 0], vb[..., 1])
        err_c = match_error(a, c, v); err_a = match_error(c, a, vb)
        sc, ec, gc = splat(v, np.where(ok_c, err_c, 1e6), t); ga = None
        sa, ea, ga = splat(vb, np.where(ok_a, err_a, 1e6), 1 - t)
        gc = gc & (ec < 1e5); ga = ga & (ea < 1e5)
        Ac = moved(a, sc, t); Cc = moved(c, sc, -(1 - t)); Pc = (1 - t) * Ac + t * Cc
        Aa = moved(a, sa, -t); Ca = moved(c, sa, 1 - t); Pa = (1 - t) * Aa + t * Ca
        dc = box(np.abs(Ac - Cc).sum(axis=2), 3); da = box(np.abs(Aa - Ca).sum(axis=2), 3)
        both = gc & ga
        pick_c = np.where(both, dc <= da, gc)
        bi = np.where(pick_c[..., None], Pc, Pa)
        # where neither flow reached: the plain splat with filled vectors (as spl)
        none = ~(gc | ga)
        out['bi'] = np.where(none[..., None], spl, bi)
        # bi2: where both reached and agree, their mean (less noise)
        agree = both & (np.abs(Pc - Pa).sum(axis=2) < 30)
        # bi+dir: a hole takes the vector of the nearest reached pixel in one of the four directions, whichever makes the moved pair agree best (a hole is mostly background
        # uncovered, so one side's vector is right and the average of both is not)
        F = np.where(pick_c[..., None], sc, -sa); reached = gc | ga
        cands = [F * 0.0]
        for axis, rev in ((1, False), (1, True), (0, False), (0, True)):
            Fa = np.flip(F, axis) if rev else F; Ra = np.flip(reached, axis) if rev else reached
            n = Ra.shape[axis]
            pos = np.arange(n).reshape((1, n) if axis == 1 else (n, 1))
            last = np.maximum.accumulate(np.where(Ra, pos, -1), axis=axis)
            if axis == 1:
                src = np.take_along_axis(Fa, np.clip(last, 0, n - 1)[..., None].repeat(2, axis=2), axis=1)
            else:
                src = np.take_along_axis(Fa, np.clip(last, 0, n - 1)[..., None].repeat(2, axis=2), axis=0)
            src = np.where((last >= 0)[..., None], src, 0.0)
            cands.append(np.flip(src, axis) if rev else src)
        best_e = np.full(a.shape[:2], 1e9, np.float32); best_u = np.zeros_like(F)
        for u in cands:
            e = box(np.abs(moved(a, u, t) - moved(c, u, -(1 - t))).sum(axis=2), 5)
            better = e < best_e
            best_u = np.where(better[..., None], u, best_u); best_e = np.where(better, e, best_e)
        hole_pair = (1 - t) * moved(a, best_u, t) + t * moved(c, best_u, -(1 - t))
        out['bi+dir'] = np.where(none[..., None], hole_pair, bi)
        # bi+all: the same candidates for every pixel, the pixel's own vector (where it has one) kept unless another one agrees clearly better
        own_e = box(np.abs(moved(a, F, t) - moved(c, F, -(1 - t))).sum(axis=2), 5)
        all_e = np.where(reached, own_e - args_margin[0], 1e9); all_u = np.where(reached[..., None], F, 0.0)
        for u in cands:
            e = box(np.abs(moved(a, u, t) - moved(c, u, -(1 - t))).sum(axis=2), 5)
            better = e < all_e
            all_u = np.where(better[..., None], u, all_u); all_e = np.where(better, e, all_e)
        out['bi+all'] = (1 - t) * moved(a, all_u, t) + t * moved(c, all_u, -(1 - t))
        # bi+conf: the final picture blended toward the unmoved mean by how little the moved pair agrees there (soft, a hedge where the motion is doubtful)
        fin = np.where(none[..., None], hole_pair, bi)
        pu = np.where(none[..., None], best_u, np.where(pick_c[..., None], sc, -sa))
        pe = box(np.abs(moved(a, pu, t) - moved(c, pu, -(1 - t))).sum(axis=2), 5)
        for name, lo, hi, cap in (('bi+conf1', 30.0, 120.0, 0.5), ('bi+conf2', 60.0, 200.0, 0.5), ('bi+conf3', 30.0, 120.0, 0.8)):
            w = np.clip((pe - lo) / (hi - lo), 0, 1)[..., None] * cap
            out[name] = (1 - w) * fin + w * out['blend']
        out['bi2'] = np.where(agree[..., None], 0.5 * (Pc + Pa), out['bi'])
        out['_ok'] = (float(ok_c.mean()), float(ok_a.mean()), float(gc.mean()), float(ga.mean()), float(none.mean()))
    # spl+dir: one splat (no second flow), the holes filled by direction
    out['spl+dir'] = dirfill_pair(vs_raw, got, a, c, t, spl)
    # med3: the middle value per channel of (moved a, moved c, the unmoved mean): one wrong candidate cannot win
    out['med3'] = np.median(np.stack([ca, cc, out['blend']], axis=0), axis=0)
    # med5: the middle of (moved a, moved c, their mean, a, c)
    out['med5'] = np.median(np.stack([ca, cc, spl, a, c], axis=0), axis=0)
    out['_cands'] = [out['blend'], a, c, spl, ca, cc, cand, out['ref']] + ([out['bi']] if 'bi' in out else [])
    out['_bad'] = float(bad.mean()); out['_got'] = float(got.mean()); out['_still'] = float(still.mean())
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('folder')
    ap.add_argument('--scale', type=int, default=4)
    ap.add_argument('--only', type=int, default=0, help='the first N triples only')
    ap.add_argument('--t', type=float, default=-1, help='the time of the frame between, 0..1 (default: the one in the file, from the capture times)')
    ap.add_argument('--auto-t', action='store_true', help='per triple the time (0.2..0.8) at which the plain moved pair is closest to the truth: the game own pacing is uneven, the capture clock does not show it; the same t for every method')
    ap.add_argument('--cubic', action='store_true', help='Catmull-Rom sampling in place of bilinear')
    ap.add_argument('--invert', action='store_true', help='the vectors the other way made by inverting this way (no second estimate), not the dumped ones')
    ap.add_argument('--agree', type=int, default=5, help='the window of the hole fill agreement test')
    ap.add_argument('--tau', type=float, default=1.0)
    ap.add_argument('--thr-pair', type=float, default=60.0)
    ap.add_argument('--thr-poor', type=float, default=90.0)
    ap.add_argument('--still', type=float, default=24.0)
    ap.add_argument('--margin', type=float, default=10.0)
    args = ap.parse_args()
    args_margin[0] = args.margin
    AGREE_BOX[0] = args.agree
    global SAMPLER
    if args.cubic:
        SAMPLER = bicubic
    files = sorted(glob.glob(os.path.join(args.folder, 'triple_*.bin')))
    if args.only:
        files = files[:args.only]
    if not files:
        sys.exit('no triple_*.bin in the folder (nr_fgeval ... flowdump=N)')
    sums = {}; stats = []
    for p in files:
        a, b, c, v, t, vb = load_triple(p, args.scale)
        if args.t >= 0: t = args.t
        if args.auto_t:
            t = max((0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8), key=lambda q: psnr((1 - q) * moved(a, v, q) + q * moved(c, v, -(1 - q)), b))
        if args.invert:   # the other way made from this way: each pixel of c's vector put on the pixel of a it points to (the best match wins), reversed; no second estimate
            iv, _, got_i = splat(v, match_error(a, c, v), 0.0)
            vb = fill_holes(-iv, got_i, -v)
        m = methods(a, c, v, t, args.thr_pair, args.thr_poor, args.still, vb, args.tau)
        okst = m.pop('_ok', None)
        if okst and not stats: print('  consistent shares (c, a), reached shares (c, a, none): %s' % ' '.join('%.2f' % x for x in okst))
        cands = m.pop('_cands'); bad = m.pop('_bad'); got = m.pop('_got'); still = m.pop('_still')
        errs = np.stack([np.abs(x - b).sum(axis=2) for x in cands], axis=0)
        m['oracle'] = np.take_along_axis(np.stack(cands, axis=0), np.argmin(errs, axis=0)[None, ..., None], axis=0)[0]
        row = {k: psnr(np.clip(x, 0, 255), b) for k, x in m.items()}
        for k, x in row.items():
            sums.setdefault(k, []).append(x)
        stats.append((bad, got, still))
        print('%s t=%.2f  ' % (os.path.basename(p), t) + '  '.join('%s %.2f' % (k, x) for k, x in row.items()), flush=True)
    s = np.array(stats).mean(axis=0)
    print('%d triples, frames %dx%d; share of pixels: splat landed %.0f%%, still %.0f%%, moved pair disagreed %.0f%%' % (len(files), a.shape[1], a.shape[0], 100 * s[1], 100 * s[2], 100 * s[0]))
    for k in ('blend', 'half', 'spl', 'spl+st', 'spl+occ', 'spl+rel', 'cand', 'ref', 'spl+dir', 'bi', 'bi+dir', 'bi+all', 'bi+conf1', 'bi+conf2', 'bi+conf3', 'bi2', 'med3', 'med5', 'oracle'):
        x = np.array(sums[k])
        print('  %-8s mean %.2f dB  (min %.2f, max %.2f)' % (k, x.mean(), x.min(), x.max()))


if __name__ == '__main__':
    main()
