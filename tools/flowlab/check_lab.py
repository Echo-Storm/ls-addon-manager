"""check_lab.py RUN_FOLDER [pairs=12] [w=1280 h=720] [slack=15] [away=1.5]: does a way-back check make the flow better? (board E12)

For each pixel of the later frame C with its vector F (to where it was in the earlier frame A), the way back is the other field's vector at that end point: a vector that is right
comes home (|F + back| small). Where it does not (more than `away` pixels plus a tenth of its length), the pixel takes instead, among 33 neighbours' vectors (the 8 around, 24 two,
four and eight pixels out) and none, the best one that does come home and carries the pixel's 3 x 3 surroundings within `slack` levels of what its own vector did; where none does,
it keeps its own (something uncovered has nowhere to come home to). The idea is from the MFG Super Motion engine's text (docs/mfg-super-motion-engine-notes.md, step 6), written
here from its description. Scored by the frame half way, drawn from one field (`oneway`) and from both (`direct`), against the true frame: PSNR gain over the plain blend.
"""
import sys
import numpy as np
from synth_lab import sample, luma, box, psnr, warp_pair, mismatch

RING = [(-1, -1), (0, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (0, 1), (1, 1)]
FAR = [(dx * s, dy * s) for s in (2, 4, 8) for dx, dy in RING]


def shift(F, dx, dy):
    return np.roll(np.roll(F, -dy, 0), -dx, 1)


def splat_back(F, gx, gy):
    """the other field made from this one alone, as EchoFG's pipeline does it: each pixel's vector, reversed, written where it lands in the other frame (the last write wins); a mask of where something landed"""
    h, w = F.shape[:2]
    px = np.clip(np.rint(gx + F[..., 0]).astype(np.int32), 0, w - 1); py = np.clip(np.rint(gy + F[..., 1]).astype(np.int32), 0, h - 1)
    back = np.zeros_like(F); valid = np.zeros((h, w), bool)
    back[py.ravel(), px.ravel()] = -F.reshape(-1, 2); valid[py.ravel(), px.ravel()] = True
    return back, valid


def check(F, back, src, dst, gx, gy, slack, away, valid=None):
    """F: the field at the pixels of `dst` pointing into `src`; back: the other field at the pixels of `src` (pointing into `dst`). All luma/float32."""
    h, w = F.shape[:2]

    def comes_home(v):
        px = np.clip(np.rint(gx + v[..., 0]).astype(np.int32), 0, w - 1); py = np.clip(np.rint(gy + v[..., 1]).astype(np.int32), 0, h - 1)
        b = back[py, px]
        m = np.hypot(v[..., 0] + b[..., 0], v[..., 1] + b[..., 1])
        return m if valid is None else np.where(valid[py, px], m, 0.0)   # where nothing landed there is nothing to check against: it passes

    def cost(v):
        return box(np.abs(luma(sample(src, gx + v[..., 0], gy + v[..., 1])) - luma(dst)), 3)

    own_away = comes_home(F)
    bad = own_away > away + 0.1 * np.hypot(F[..., 0], F[..., 1])
    if not bad.any():
        return F, 0.0
    limit = cost(F) + slack
    best = np.full(F.shape[:2], 1e30, np.float32); found = F.copy(); changed = np.zeros(F.shape[:2], bool)
    for k, off in enumerate(RING + FAR + [None]):
        v = np.zeros_like(F) if off is None else shift(F, off[0], off[1])
        ok = comes_home(v) <= away + 0.1 * np.hypot(v[..., 0], v[..., 1])
        c = cost(v)
        take = bad & ok & (c < limit) & (c < best)
        best = np.where(take, c, best); found = np.where(take[..., None], v, found); changed |= take
    return found, float(changed.mean())


def main():
    folder = sys.argv[1]
    args = {k: v for k, v in (s.split('=') for s in sys.argv[2:])}
    w = int(args.get('w', 1280)); h = int(args.get('h', 720))
    slack = float(args.get('slack', 15)); away = float(args.get('away', 1.5))
    frames = np.memmap(folder + '/frames.rgba', np.uint8, 'r').reshape(-1, h, w, 4)
    Fc_all = np.memmap(folder + '/ours.f32', np.float32, 'r').reshape(-1, h, w, 2)
    Fa_all = np.memmap(folder + '/ours_rev.f32', np.float32, 'r').reshape(-1, h, w, 2)
    pairs = min(len(Fc_all), len(Fa_all), int(args.get('pairs', 12)))
    gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    border = (slice(24, h - 24), slice(24, w - 24))
    names = ['blend', 'oneway', 'oneway+check', 'direct', 'direct+check']
    res = {n: [] for n in names}; share = []
    for p in range(pairs):
        r = p + 1
        A = frames[2 * (r - 1), :, :, :3].astype(np.float32); C = frames[2 * r, :, :, :3].astype(np.float32); T = frames[2 * r - 1, :, :, :3].astype(np.float32)
        Fc = Fc_all[p].astype(np.float32); Fa = Fa_all[p].astype(np.float32)
        if args.get('reverse', 'real') == 'splat':   # the way back made from the forward field alone (what EchoFG already has), not a second estimate
            Fb, vb = splat_back(Fc, gx, gy)
            Fc2, s1 = check(Fc, Fb, A, C, gx, gy, slack, away, vb); Fa2, s2 = Fa, 0.0
        else:
            Fc2, s1 = check(Fc, Fa, A, C, gx, gy, slack, away)
            Fa2, s2 = check(Fa, Fc, C, A, gx, gy, slack, away)
        share.append((s1, s2))
        out = {'blend': 0.5 * (A + C)}
        for name, F in (('oneway', Fc), ('oneway+check', Fc2)):
            out[name] = 0.5 * (sample(A, gx + 0.5 * F[..., 0], gy + 0.5 * F[..., 1]) + sample(C, gx - 0.5 * F[..., 0], gy - 0.5 * F[..., 1]))
        for name, (fa, fc) in (('direct', (Fa, Fc)), ('direct+check', (Fa2, Fc2))):
            f = fa; b = -fc
            agree = np.hypot(*(f - b).transpose(2, 0, 1)) <= 1.0 + 0.2 * np.hypot(f[..., 0], f[..., 1])
            mf, mb = mismatch(A, C, f, gx, gy), mismatch(A, C, b, gx, gy)
            v = np.where(agree[..., None], 0.5 * (f + b), np.where((mf <= mb)[..., None], f, b))
            a, c = warp_pair(A, C, v, gx, gy); out[name] = 0.5 * (a + c)
        for n in names:
            res[n].append(psnr(out[n][border], T[border]))
    base = np.array(res['blend'])
    print('%d pairs (slack %g, away %g): replaced %.1f %% of C\'s pixels, %.1f %% of A\'s' % (pairs, slack, away, 100 * np.mean([s[0] for s in share]), 100 * np.mean([s[1] for s in share])))
    for n in names:
        a = np.array(res[n])
        print('%-13s %6.2f dB   gain over blend %+.2f dB' % (n, a.mean(), (a - base).mean()))


if __name__ == '__main__':
    main()
