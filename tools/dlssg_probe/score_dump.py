"""score_dump.py DUMP.raw...: the same correlation score as check_clip.py, for the frames nr_fgeval rebuilt (dumpraw=FILE): FSR's frame generation, or EchoFG's own synthesis.

Each entry of a dump is a frame number and four 1280 x 720 RGBA pictures: the rebuilt frame, the true frame, the real frame before it and the one after. Score = the
correlation of the brightness with the true frame (1.0 = the same picture), for the rebuilt frame and for the two real frames blended half and half. No brightness mapping is
needed here (the pictures never went through a screen)."""
import sys
import numpy as np

W, H = 1280, 720
ENTRY = 4 + 4 * W * H * 4


def luma(a):
    return a[..., 0].astype(np.float32) * 0.299 + a[..., 1].astype(np.float32) * 0.587 + a[..., 2].astype(np.float32) * 0.114


def ncc(a, b):
    a = a - a.mean(); b = b - b.mean()
    return float((a * b).sum() / (np.sqrt((a * a).sum() * (b * b).sum()) + 1e-9))


for path in sys.argv[1:]:
    raw = np.memmap(path, np.uint8, 'r')
    n = len(raw) // ENTRY
    gen, blend, hold = [], [], []
    for e in range(n):
        o = e * ENTRY + 4
        pics = [luma(raw[o + k * W * H * 4:o + (k + 1) * W * H * 4].reshape(H, W, 4)) for k in range(4)]
        t = pics[1]
        gen.append(ncc(pics[0], t)); blend.append(ncc(0.5 * (pics[2] + pics[3]), t)); hold.append(ncc(pics[2], t))
    g, b, h = np.array(gen), np.array(blend), np.array(hold)
    print('%-52s %d frames: generated %.4f  blend %.4f  hold %.4f  | gain over blend %+.4f  beats the blend in %d' % (path.replace(chr(92), '/').split('/')[-1], n, g.mean(), b.mean(), h.mean(), (g - b).mean(), int((g > b).sum())))
