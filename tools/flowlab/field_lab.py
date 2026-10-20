"""field_lab.py RUN_FOLDER [pairs=8] [w=1280 h=720] [only=name,name]: three ideas for the flow field, each applied to both fields and scored by the frame drawn between (board E15, E16, E17).

  smooth   (E17) outlier replacement (a vector that stands alone where 7 of its 8 neighbours agree takes theirs), then each vector becomes the mean of the 5 x 5 around it weighted
           by how near each is to its own (a Gaussian on the vector difference, wider the longer the vector) and halved at the outer ring: no steps between neighbours
  large    (E16) the picture at large: the mean of the moving, well matched vectors over 41 x 41 pixels (a second round counts those near the mean most); a pixel that shows little
           (low local contrast) takes it where the two frames agree under it about as well as under the pixel's own vector, and a pixel whose own vector does not carry it takes it
           where it carries it clearly better
  far      (E15) a pixel whose vector leaves its surroundings badly matched although they show something is looked for far and wide (every second pixel within 24 around its own
           vector), then one pixel around the best; taken only where that fits clearly better
Drawn from both fields (`direct`: the movement at the pixel from the two fields, drawn straight from the real frames) and from one (`oneway`); PSNR gain over the plain blend.
Ideas from the MFG Super Motion engine's text (docs/mfg-super-motion-engine-notes.md, steps 5, 8 and 10), written here from their descriptions.
"""
import sys
import numpy as np
from synth_lab import sample, luma, box, psnr, warp_pair, mismatch


def cost(F, src, dst, gx, gy, k=3):
    return box(np.abs(luma(sample(src, gx + F[..., 0], gy + F[..., 1])) - luma(dst)), k)


def smooth(F, src, dst, gx, gy):
    h, w = F.shape[:2]
    # outlier replacement
    ring = [np.roll(np.roll(F, -dy, 0), -dx, 1) for dy in (-1, 0, 1) for dx in (-1, 0, 1) if dx or dy]
    med = np.median(np.stack(ring), axis=0)
    near = 0.75 + 0.15 * np.hypot(med[..., 0], med[..., 1])
    agree = sum((np.hypot(r[..., 0] - med[..., 0], r[..., 1] - med[..., 1]) <= near).astype(np.int32) for r in ring)
    own_far = np.hypot(F[..., 0] - med[..., 0], F[..., 1] - med[..., 1]) > near
    G = np.where(((agree >= 7) & own_far)[..., None], med, F)
    # the similarity-weighted 5 x 5 mean
    spread = 0.75 + 0.15 * np.hypot(G[..., 0], G[..., 1])
    num = np.zeros_like(G); den = np.zeros(G.shape[:2], np.float32)
    for dy in range(-2, 3):
        for dx in range(-2, 3):
            O = np.roll(np.roll(G, -dy, 0), -dx, 1)
            d2 = ((O - G) ** 2).sum(-1)
            wt = np.exp(-d2 / (2 * spread ** 2)) * (0.5 if (abs(dx) == 2 or abs(dy) == 2) else 1.0)
            num += O * wt[..., None]; den += wt
    return num / den[..., None]


def large_field(F, src, dst, gx, gy, size=41):
    c = cost(F, src, dst, gx, gy)
    moving = np.hypot(F[..., 0], F[..., 1]) > 1.0
    wgt = (moving & (c < 12.0)).astype(np.float32)
    ws = np.maximum(box(wgt, size), 1e-3)
    L = np.stack([box(F[..., 0] * wgt, size), box(F[..., 1] * wgt, size)], -1) / ws[..., None]
    for _ in range(2):   # those near the mean count most
        d = np.hypot(F[..., 0] - L[..., 0], F[..., 1] - L[..., 1]) / (1.0 + 0.15 * np.hypot(L[..., 0], L[..., 1]))
        w2 = wgt / ((1 + d * d) ** 2)
        s2 = np.maximum(box(w2, size), 1e-3)
        L = np.stack([box(F[..., 0] * w2, size), box(F[..., 1] * w2, size)], -1) / s2[..., None]
    return np.where((ws > 0.05)[..., None], L, 0.0), ws


DEBUG = {}
LARGE = (1.25, 4.0, 1e9, 3.0, 2.5, 8.0, 8.0, 2.0)   # the picture at large wins where its cost is within this multiple of the pixel's own and this many levels more


def large(F, src, dst, gx, gy):
    """the picture at large wins ties: where its vector carries the pixel about as well as the pixel's own (within a few levels and a quarter), and differs, it is taken;
    where the pixel's own carries it clearly better (a thing of its own: the pole) the own stays"""
    L, ws = large_field(F, src, dst, gx, gy, 129)
    own = cost(F, src, dst, gx, gy)
    lc = cost(L, src, dst, gx, gy)
    differs = np.hypot(L[..., 0] - F[..., 0], L[..., 1] - F[..., 1]) > 1.0
    # only where the pixel's own vector is ambiguous: the same patch matches about as well a few pixels further along the vector's own line or across it (repeating lines, plain)
    amb = np.zeros(own.shape, bool)
    for dx, dy in ((3, 0), (-3, 0), (5, 0), (-5, 0), (7, 0), (-7, 0), (0, 3), (0, -3), (0, 5), (0, -5), (0, 7), (0, -7)):
        amb |= cost(F + np.array([dx, dy], np.float32), src, dst, gx, gy) <= own + LARGE[3]
    take = differs & (ws > 0.05) & amb & (lc <= own * LARGE[0] + LARGE[1])
    return np.where(take[..., None], L, F), float(take.mean())


def far(F, src, dst, gx, gy, reach=24):
    h, w = F.shape[:2]
    own = cost(F, src, dst, gx, gy)
    contrast = box(np.abs(luma(dst) - box(luma(dst), 7)), 7)
    bad = (own > 14.0) & (contrast > 4.0)
    ys, xs = np.nonzero(bad)
    if len(ys) == 0:
        return F, 0.0
    v0 = F[ys, xs]; best = own[ys, xs].copy(); found = v0.copy()
    ld = luma(dst); ls = luma(src)
    def patch_cost(v):
        tot = 0
        for oy in (-1, 0, 1):
            for ox in (-1, 0, 1):
                px = np.clip(xs + ox + v[:, 0], 0, w - 1.001); py = np.clip(ys + oy + v[:, 1], 0, h - 1.001)
                x0 = px.astype(np.int32); y0 = py.astype(np.int32); fx = px - x0; fy = py - y0
                a = ls[y0, x0] * (1 - fx) * (1 - fy) + ls[y0, x0 + 1] * fx * (1 - fy) + ls[y0 + 1, x0] * (1 - fx) * fy + ls[y0 + 1, x0 + 1] * fx * fy
                tot = tot + np.abs(a - ld[np.clip(ys + oy, 0, h - 1), np.clip(xs + ox, 0, w - 1)])
        return tot / 9.0
    for dy in range(-reach, reach + 1, 2):
        for dx in range(-reach, reach + 1, 2):
            v = v0 + np.array([dx, dy], np.float32)
            c = patch_cost(v)
            m = c < best
            best = np.where(m, c, best); found = np.where(m[:, None], v, found)
    centre = found.copy()
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            v = centre + np.array([dx, dy], np.float32)
            c = patch_cost(v)
            m = c < best
            best = np.where(m, c, best); found = np.where(m[:, None], v, found)
    take = best < own[ys, xs] * 0.5 - 2.0
    G = F.copy(); G[ys[take], xs[take]] = found[take]
    return G, float(take.sum()) / (h * w)


def tile_modes(F, good, tile=32, reach=3):
    """the dominant vector of each tile and of the tiles around it (a window of 2 * reach + 1 tiles): the mode of the good pixels' vectors on a half-pixel grid, then the mean of
    those within one pixel of it; and the share of the window's good pixels that agree with it"""
    h, w = F.shape[:2]
    ty, tx = (h + tile - 1) // tile, (w + tile - 1) // tile
    modes = np.zeros((ty, tx, 2), np.float32); support = np.zeros((ty, tx), np.float32)
    q = np.rint(F * 2.0).astype(np.int32)
    for j in range(ty):
        for i in range(tx):
            y0, y1 = max(0, (j - reach) * tile), min(h, (j + reach + 1) * tile); x0, x1 = max(0, (i - reach) * tile), min(w, (i + reach + 1) * tile)
            g = good[y0:y1:2, x0:x1:2]
            if g.sum() < 60:
                continue
            v = q[y0:y1:2, x0:x1:2][g]
            key = (v[:, 0] + 512) * 2048 + (v[:, 1] + 512)
            u, inv, cnt = np.unique(key, return_inverse=True, return_counts=True)
            # a vector's vote counts its neighbours at half a pixel too
            best = None
            top = np.argsort(cnt)[::-1][:6]
            for k in top:
                c = v[inv == k][0]
                near = (np.abs(v[:, 0] - c[0]) <= 2) & (np.abs(v[:, 1] - c[1]) <= 2)
                if best is None or near.sum() > best[0]:
                    best = (near.sum(), near, c)
            near = best[1]
            vv = (F[y0:y1:2, x0:x1:2][g])[near]
            modes[j, i] = vv.mean(0); support[j, i] = near.sum() / g.sum()
    return modes, support


def large2(F, src, dst, gx, gy):
    h, w = F.shape[:2]
    own = cost(F, src, dst, gx, gy)
    contrast = box(np.abs(luma(dst) - box(luma(dst), 9)), 9)
    good = (own < 10.0) & (contrast > 3.0) & (np.hypot(F[..., 0], F[..., 1]) > 1.0)    # found, showing something, and moving
    modes, support = tile_modes(F, good)
    tile = 32
    ti = np.clip((gy // tile).astype(np.int32), 0, modes.shape[0] - 1); tj = np.clip((gx // tile).astype(np.int32), 0, modes.shape[1] - 1)
    L = modes[ti, tj]; sup = support[ti, tj]
    lc = cost(L, src, dst, gx, gy)
    differs = np.hypot(L[..., 0] - F[..., 0], L[..., 1] - F[..., 1]) > 1.0
    # ambiguous: flat, or a patch that is matched about as well at a distance of 6 to 16 pixels along or across (repeating)
    flat = contrast < LARGE[4]
    rep = np.zeros(own.shape, bool)
    for d in (6, 8, 10, 12, 14, 16):
        for dx, dy in ((d, 0), (-d, 0), (0, d), (0, -d)):
            rep |= cost(F + np.array([dx, dy], np.float32), src, dst, gx, gy) <= own + 0.15 * contrast + 0.5
    # (a vector that is matched badly can look 'ambiguous' only because every nearby vector is matched badly: only a pixel that matches well can be ambiguous)
    ambiguous = (flat | rep) & (own < LARGE[5]) & (lc <= own * LARGE[0] + LARGE[1])
    clearly = lc + LARGE[6] < own                    # the picture's vector carries the pixel clearly better than its own
    # a vector faster than the picture at large is not an alias of it: it is a thing of its own (a pole, a character) and stays
    slower = np.hypot(F[..., 0], F[..., 1]) <= np.hypot(L[..., 0], L[..., 1]) + LARGE[7]
    take = differs & (sup > 0.3) & ((ambiguous & slower) | clearly)
    DEBUG.update(dict(ambiguous=differs & (sup > 0.3) & ambiguous, clearly=differs & (sup > 0.3) & clearly & ~ambiguous, own=own, lc=lc, L=L))
    return np.where(take[..., None], L, F), float(take.mean())


METHODS = {'large2': large2, 'smooth': lambda F, s, d, gx, gy: (smooth(F, s, d, gx, gy), 0.0), 'large': large, 'far': far}


def draw(A, C, Fa, Fc, gx, gy):
    f = Fa; b = -Fc
    agree = np.hypot(*(f - b).transpose(2, 0, 1)) <= 1.0 + 0.2 * np.hypot(f[..., 0], f[..., 1])
    mf, mb = mismatch(A, C, f, gx, gy), mismatch(A, C, b, gx, gy)
    v = np.where(agree[..., None], 0.5 * (f + b), np.where((mf <= mb)[..., None], f, b))
    a, c = warp_pair(A, C, v, gx, gy)
    one = 0.5 * (sample(A, gx + 0.5 * Fc[..., 0], gy + 0.5 * Fc[..., 1]) + sample(C, gx - 0.5 * Fc[..., 0], gy - 0.5 * Fc[..., 1]))
    return 0.5 * (a + c), one


def main():
    folder = sys.argv[1]
    args = {k: v for k, v in (s.split('=') for s in sys.argv[2:])}
    w = int(args.get('w', 1280)); h = int(args.get('h', 720))
    only = args.get('only', 'smooth,large,far').split(',')
    global LARGE
    LARGE = tuple(float(x) for x in args.get('large', '1.1,1.5,1e9,3,2.5,8,8,2').split(','))
    frames = np.memmap(folder + '/frames.rgba', np.uint8, 'r').reshape(-1, h, w, 4)
    Fc_all = np.memmap(folder + '/ours.f32', np.float32, 'r').reshape(-1, h, w, 2)
    Fa_all = np.memmap(folder + '/ours_rev.f32', np.float32, 'r').reshape(-1, h, w, 2)
    pairs = min(len(Fc_all), len(Fa_all), int(args.get('pairs', 8)))
    gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    border = (slice(24, h - 24), slice(24, w - 24))
    import os
    masks_t = dict(np.load(folder + '/masks_t.npz')) if os.path.exists(folder + '/masks_t.npz') else {}
    masks = {m: v[0] for m, v in masks_t.items()}
    reg = {m: {} for m in masks}   # region -> method -> list of PSNR (direct)
    names = ['blend', 'base'] + only
    res = {n: [] for n in names}; res1 = {n: [] for n in names}; shares = {n: [] for n in only}
    for p in range(pairs):
        r = p + 1
        A = frames[2 * (r - 1), :, :, :3].astype(np.float32); C = frames[2 * r, :, :, :3].astype(np.float32); T = frames[2 * r - 1, :, :, :3].astype(np.float32)
        Fc = Fc_all[p].astype(np.float32); Fa = Fa_all[p].astype(np.float32)
        d0, o0 = draw(A, C, Fa, Fc, gx, gy)
        blend = 0.5 * (A + C)
        res['blend'].append(psnr(blend[border], T[border])); res1['blend'].append(psnr(blend[border], T[border]))
        res['base'].append(psnr(d0[border], T[border])); res1['base'].append(psnr(o0[border], T[border]))
        def regional(name, img):
            e = ((img - T) ** 2).mean(-1)
            for m, mkall in masks_t.items():
                mk = mkall[r - 1]
                reg[m].setdefault(name, []).append(10 * np.log10(255 * 255 / max(float(e[mk & (np.arange(h)[:, None] > 24) & (np.arange(w)[None, :] > 24)].mean()), 1e-6)))
        regional('blend', blend); regional('base', d0); regional('base/1', o0)
        for n in only:
            Fc2, s1 = METHODS[n](Fc, A, C, gx, gy)            # the field at C into A
            Fa2, s2 = METHODS[n](Fa, C, A, gx, gy)            # the field at A into C
            shares[n].append(0.5 * (s1 + s2))
            d, o = draw(A, C, Fa2, Fc2, gx, gy)
            res[n].append(psnr(d[border], T[border])); res1[n].append(psnr(o[border], T[border])); regional(n, d); regional(n + '/1', o)
    base = np.array(res['blend'])
    print('%d pairs: gain over the blend, both fields (direct) / one field (oneway)' % pairs)
    for n in names:
        a = np.array(res[n]); b = np.array(res1[n]); bb = np.array(res1['blend'])
        extra = '   changed %.1f %% of the pixels' % (100 * np.mean(shares[n])) if n in shares else ''
        print('%-7s direct %+.2f dB   oneway %+.2f dB%s' % (n, (a - base).mean(), (b - bb).mean(), extra))
    for m in masks:
        bl = np.array(reg[m]['blend'])
        print('  region %-9s (%4.1f %% of the picture): ' % (m, 100 * masks[m].mean()) + '   '.join('%s %+.2f dB' % (n, (np.array(reg[m][n]) - bl).mean()) for n in (['base'] + only + ['base/1'] + [x + '/1' for x in only]) if n != 'blend'))


if __name__ == '__main__':
    main()
