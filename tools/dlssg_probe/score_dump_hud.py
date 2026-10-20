"""score_dump_hud.py DUMP.raw...: nr_fgeval's rebuilt frames scored on a HUD that stays put against the rest of the picture (PSNR, dB).

The same dump files as score_dump.py (per frame: its number, then 1280 x 720 RGBA of the rebuilt picture, the true frame, the real frame before and the one after;
nr_fgeval dumpraw=FILE dumpx=N dumpy=N chooses which piece of the frame). The HUD is the set of pixels that never change (less than 10 levels of brightness) across the
real frames of the whole run: a mask for scoring only. Each file is scored on the HUD pixels and on the rest, beside the plain blend of the two real frames."""
import sys
import numpy as np

W, H = 1280, 720
ENTRY = 4 + 4 * W * H * 4


def luma(a):
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


def entry(raw, e):
    o = e * ENTRY + 4
    return [raw[o + k * W * H * 4:o + (k + 1) * W * H * 4].reshape(H, W, 4)[:, :, :3].astype(np.float32) for k in range(4)]


def main():
    border = np.zeros((H, W), bool); border[12:H - 12, 12:W - 12] = True
    for path in sys.argv[1:]:
        raw = np.memmap(path, np.uint8, 'r')
        n = len(raw) // ENTRY
        lum = np.stack([luma(entry(raw, e)[2]) for e in range(n)])
        still = ((lum.max(0) - lum.min(0)) < 10) & border
        rest = ~still & border
        g_h, g_r, b_h, b_r = [], [], [], []
        for e in range(n):
            pic, truth, a, c = entry(raw, e)
            for img, hs, rs in ((pic, g_h, g_r), (0.5 * (a + c), b_h, b_r)):
                err = ((img - truth) ** 2).mean(-1)
                hs.append(10 * np.log10(255 * 255 / max(err[still].mean(), 1e-6)) if still.any() else 99.0)
                rs.append(10 * np.log10(255 * 255 / max(err[rest].mean(), 1e-6)))
        print('%-44s %2d frames, HUD %4.1f %%:  rebuilt HUD %6.2f dB rest %6.2f dB   (blend HUD %6.2f rest %6.2f)' % (path.replace(chr(92), '/').split('/')[-1], n, 100 * still.mean(),
              np.mean(g_h), np.mean(g_r), np.mean(b_h), np.mean(b_r)))


main()
