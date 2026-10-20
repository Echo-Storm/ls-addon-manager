"""synth_lab.py RUN_FOLDER [w=1280 h=720] [pairs=29]: how to draw the frame between two real frames from two motion fields, tried several ways on real footage with the truth known.

Input: what flowlab.exe wrote: frames.rgba (even frames real, odd frames the truth between), ours.f32 (the flow AT the later frame C pointing back to the earlier A: x_in_A =
x + Fc(x)) and ours_rev.f32 (the flow AT A pointing forward to C: x_in_C = x + Fa(x)). All the methods make the frame half way (t = 0.5) and are scored by PSNR and by the
correlation of brightness against the true frame.

Methods
  blend      the two frames averaged (no motion)
  oneway     0.5 * (A(x + Fc/2) + C(x - Fc/2)): one field, read as if it were the middle frame's (the flowlab scorer's way)
  direct     the motion AT the pixel being made, from both fields: where they agree (the way back comes home within a texel and 20 %) their mean, else the better-fitting one;
             drawn straight from the two real frames, nothing carried (idea: docs/mfg-super-motion-study.md, "direct")
  fit        direct, then per pixel the best of a few candidate motions (the mean, each field's own, the 8 neighbours' mean, none) by how well the two real frames agree under it
  fit+soft   fit, then where the frames still disagree under the chosen motion the picture leans to the plain blend, as a soft trace, not a piece of the wrong frame
"""
import sys
import numpy as np


def sample(img, xs, ys):
    h, w = img.shape[:2]
    xs = np.clip(xs, 0, w - 1.001); ys = np.clip(ys, 0, h - 1.001)
    x0 = xs.astype(np.int32); y0 = ys.astype(np.int32)
    fx = (xs - x0)[..., None]; fy = (ys - y0)[..., None]
    a = img[y0, x0] * (1 - fx) + img[y0, x0 + 1] * fx
    b = img[y0 + 1, x0] * (1 - fx) + img[y0 + 1, x0 + 1] * fx
    return a * (1 - fy) + b * fy


def luma(a):
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


def box(a, k):
    r = k // 2
    p = np.pad(a, r, mode='edge')
    c = np.pad(np.cumsum(np.cumsum(p, 0), 1), ((1, 0), (1, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def ncc(a, b):
    a = a - a.mean(); b = b - b.mean()
    return float((a * b).sum() / (np.sqrt((a * a).sum() * (b * b).sum()) + 1e-9))


def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    return 99.0 if mse < 1e-9 else 10 * np.log10(255.0 * 255.0 / mse)


def warp_pair(A, C, v, gx, gy, t=0.5):
    """the frame at time t from A and C under the movement v (A -> C, pixels) at each pixel of the frame being made"""
    return sample(A, gx - t * v[..., 0], gy - t * v[..., 1]), sample(C, gx + (1 - t) * v[..., 0], gy + (1 - t) * v[..., 1])


def mismatch(A, C, v, gx, gy, k=5):
    a, c = warp_pair(A, C, v, gx, gy)
    return box(np.abs(luma(a) - luma(c)), k)


def main():
    folder = sys.argv[1]
    args = {k: v for k, v in (s.split('=') for s in sys.argv[2:])}
    w = int(args.get('w', 1280)); h = int(args.get('h', 720))
    frames = np.memmap(folder + '/frames.rgba', np.uint8, 'r').reshape(-1, h, w, 4)
    Fc_all = np.memmap(folder + '/ours.f32', np.float32, 'r').reshape(-1, h, w, 2)
    Fa_all = np.memmap(folder + '/ours_rev.f32', np.float32, 'r').reshape(-1, h, w, 2)
    pairs = min(len(Fc_all), len(Fa_all), int(args.get('pairs', 99)))
    gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    border = (slice(24, h - 24), slice(24, w - 24))
    names = ['blend', 'oneway', 'direct', 'fit', 'fit+soft']
    res = {n: [] for n in names}
    for p in range(pairs):
        r = p + 1
        A = frames[2 * (r - 1), :, :, :3].astype(np.float32); C = frames[2 * r, :, :, :3].astype(np.float32); T = frames[2 * r - 1, :, :, :3].astype(np.float32)
        Fc = Fc_all[p].astype(np.float32)      # at C, to A      (movement A -> C is -Fc)
        Fa = Fa_all[p].astype(np.float32)      # at A, to C      (movement A -> C is +Fa)
        Tl = luma(T)
        out = {}
        out['blend'] = 0.5 * (A + C)
        a, c = sample(A, gx + 0.5 * Fc[..., 0], gy + 0.5 * Fc[..., 1]), sample(C, gx - 0.5 * Fc[..., 0], gy - 0.5 * Fc[..., 1])
        out['oneway'] = 0.5 * (a + c)
        f = Fa; b = -Fc                        # both as movement A -> C, each found at the place itself
        agree = np.hypot(*(f + (-b)).transpose(2, 0, 1)) <= 1.0 + 0.2 * np.hypot(f[..., 0], f[..., 1])   # |f + back| with back = Fc: the way back comes home
        mean = 0.5 * (f + b)
        mf, mb = mismatch(A, C, f, gx, gy), mismatch(A, C, b, gx, gy)
        better = np.where((mf <= mb)[..., None], f, b)
        v = np.where(agree[..., None], mean, better)
        a, c = warp_pair(A, C, v, gx, gy); out['direct'] = 0.5 * (a + c)
        # candidates: the mean, each own, the mean of the 8 neighbours' chosen word, none
        cand = [v, f, b, np.zeros_like(v)]
        nb = sum(np.roll(np.roll(v, dy, 0), dx, 1) for dy in (-1, 0, 1) for dx in (-1, 0, 1) if dx or dy) / 8.0
        cand.append(nb)
        costs = np.stack([mismatch(A, C, k_, gx, gy, 5) for k_ in cand])
        # a small preference for the first (the direct word): another must win by a margin
        costs[0] -= 1.0
        pick = costs.argmin(0)
        vf = np.choose(pick[..., None], [c_ for c_ in cand]) if False else np.take_along_axis(np.stack(cand), pick[None, ..., None].repeat(2, -1), 0)[0]
        a, c = warp_pair(A, C, vf, gx, gy); fit = 0.5 * (a + c); out['fit'] = fit
        # where they still disagree: lean to the plain blend (a soft trace)
        miss = box(np.abs(luma(a) - luma(c)), 3)
        lean = np.clip((miss - 12.0) / 30.0, 0, 0.8)[..., None]
        out['fit+soft'] = fit * (1 - lean) + out['blend'] * lean
        for n in names:
            m = out[n]
            res[n].append((psnr(m[border], T[border]), ncc(luma(m)[border], Tl[border])))
    print('%d pairs: PSNR dB / correlation' % pairs)
    base = np.array(res['blend'])
    for n in names:
        a = np.array(res[n])
        print('%-9s %6.2f dB  %.4f   gain over blend %+.2f dB  beats blend in %d of %d' % (n, a[:, 0].mean(), a[:, 1].mean(), (a[:, 0] - base[:, 0]).mean(), int((a[:, 0] > base[:, 0]).sum()), pairs))


main()
