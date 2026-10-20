"""score_flow.py RUN_FOLDER [w=1280 h=720] [flows=ours,ofa]: how good is each motion field, on real frames, with the truth known.

Reads what flowlab.exe wrote: frames.rgba (every cropped frame of the clip; the even ones are the real frames, the odd ones the truth between them) and one file of float32
flow fields per source (x, y per pixel; the flow AT the later real frame C pointing back to the earlier one A: x_in_A - x_in_C).

Two scores per source, both with the same code:
  photometric error  mean |C(x) - A(x + F(x))| over the picture (levels of 255, lower is better): does the flow find where each pixel came from;
  interpolation      the frame between A and C is made as 0.5 * (A(x + F/2) + C(x - F/2)) (the same way for every source), and compared with the true frame between:
                     PSNR (dB) and the correlation of brightness (1.0 = the same picture). Beside it: no motion at all (that is the plain blend).
OFA's sign convention is checked, not assumed: the sign with the lower photometric error is used and reported.
"""
import sys
import numpy as np


def sample(img, xs, ys):
    """bilinear sample of img (H, W, C) at float positions, edges clamped"""
    h, w = img.shape[:2]
    xs = np.clip(xs, 0, w - 1.001); ys = np.clip(ys, 0, h - 1.001)
    x0 = xs.astype(np.int32); y0 = ys.astype(np.int32)
    fx = (xs - x0)[..., None]; fy = (ys - y0)[..., None]
    a = img[y0, x0] * (1 - fx) + img[y0, x0 + 1] * fx
    b = img[y0 + 1, x0] * (1 - fx) + img[y0 + 1, x0 + 1] * fx
    return a * (1 - fy) + b * fy


def luma(a):
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


def ncc(a, b):
    a = a - a.mean(); b = b - b.mean()
    return float((a * b).sum() / (np.sqrt((a * a).sum() * (b * b).sum()) + 1e-9))


def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    return 99.0 if mse < 1e-9 else 10 * np.log10(255.0 * 255.0 / mse)


def box(a, k):
    r = k // 2
    p = np.pad(a, r, mode='edge')
    c = np.cumsum(np.cumsum(p, 0), 1)
    c = np.pad(c, ((1, 0), (1, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def main():
    folder = sys.argv[1]
    args = {k: v for k, v in (s.split('=') for s in sys.argv[2:])}
    w = int(args.get('w', 1280)); h = int(args.get('h', 720))
    names = args.get('flows', 'ours,ofa').split(',')
    frames = np.memmap(folder + '/frames.rgba', np.uint8, 'r').reshape(-1, h, w, 4)
    flows = {n: np.memmap('%s/%s.f32' % (folder, n), np.float32, 'r').reshape(-1, h, w, 2) for n in names}
    pairs = min(len(f) for f in flows.values())
    gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    border = (slice(24, h - 24), slice(24, w - 24))
    fuse = len(names) == 2 and args.get('fuse', '1') == '1'
    res = {n: [] for n in names + ['none'] + (['fused'] if fuse else [])}
    sign = {}
    for p in range(pairs):
        r = p + 1
        A = frames[2 * (r - 1), :, :, :3].astype(np.float32); C = frames[2 * r, :, :, :3].astype(np.float32); T = frames[2 * r - 1, :, :, :3].astype(np.float32)
        Tl = luma(T)
        fields = {}
        for n in names + ['none']:
            F = np.zeros((h, w, 2), np.float32) if n == 'none' else flows[n][p].astype(np.float32)
            if n != 'none' and n not in sign:   # the sign: the lower photometric error wins (decided once, on the first pair)
                e = {s: np.abs(luma(sample(A, gx + s * F[..., 0], gy + s * F[..., 1])) - luma(C))[border].mean() for s in (1.0, -1.0)}
                sign[n] = 1.0 if e[1.0] <= e[-1.0] else -1.0
                print('%s: sign %+d (photometric error %.2f with +, %.2f with -)' % (n, sign[n], e[1.0], e[-1.0]))
            F = F * (sign.get(n, 1.0))
            photo = np.abs(luma(sample(A, gx + F[..., 0], gy + F[..., 1])) - luma(C))[border].mean()
            M = 0.5 * (sample(A, gx + 0.5 * F[..., 0], gy + 0.5 * F[..., 1]) + sample(C, gx - 0.5 * F[..., 0], gy - 0.5 * F[..., 1]))
            res[n].append((photo, psnr(M[border], T[border]), ncc(luma(M)[border], Tl[border])))
            if n in names: fields[n] = F
        if fuse:   # per pixel, the vector whose 7 x 7 surroundings match the later frame better after moving the earlier one
            cost = {n: box(np.abs(luma(sample(A, gx + fields[n][..., 0], gy + fields[n][..., 1])) - luma(C)), 7) for n in names}
            pick = (cost[names[1]] < cost[names[0]])[..., None]
            F = np.where(pick, fields[names[1]], fields[names[0]])
            M = 0.5 * (sample(A, gx + 0.5 * F[..., 0], gy + 0.5 * F[..., 1]) + sample(C, gx - 0.5 * F[..., 0], gy - 0.5 * F[..., 1]))
            res['fused'].append((np.abs(luma(sample(A, gx + F[..., 0], gy + F[..., 1])) - luma(C))[border].mean(), psnr(M[border], T[border]), ncc(luma(M)[border], Tl[border])))
    print('\n%d pairs' % pairs)
    print('%-8s %22s %16s %14s' % ('source', 'photometric error', 'PSNR vs truth', 'correlation'))
    for n in ['none'] + names + (['fused'] if fuse else []):
        a = np.array(res[n])
        print('%-8s %22.3f %13.2f dB %14.4f' % ('blend' if n == 'none' else n, a[:, 0].mean(), a[:, 1].mean(), a[:, 2].mean()))
    for n in names:
        a = np.array(res[n]); b = np.array(res['none'])
        print('%s beats the blend (PSNR) in %d of %d pairs' % (n, int((a[:, 1] > b[:, 1]).sum()), pairs))
    if len(names) == 2:
        a = np.array(res[names[0]]); b = np.array(res[names[1]])
        print('%s beats %s (PSNR) in %d of %d pairs' % (names[1], names[0], int((b[:, 1] > a[:, 1]).sum()), pairs))


main()
