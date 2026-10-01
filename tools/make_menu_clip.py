"""Writes a synthetic .lsrec for ghost tests: a textured scene that drifts slowly, and a menu panel (dark, semi-transparent, with light lines)
that pops onto the screen at one frame and off at another, the way a game's menus do. The frames after the menu is gone are exactly the
frames the same clip would have had without it, so a picture that still shows the panel is a ghost.

    python make_menu_clip.py out.lsrec [--w 1920] [--h 1080] [--frames 160] [--on 40] [--off 90] [--drift 0.5] [--alpha 0.85]

Then, for example:  nr_sreval out.lsrec <folder> backend=dlss sharpen=70 steady=60 shrink=150
and read the per-frame "upscaled dB" column: it drops while the menu is up (the menu is not in the stretched frames either way: it is in the
frames themselves) and must come back to the level from before the menu within a few frames of the "off" frame.
"""
import argparse
import struct
import numpy as np


def scene(w, h, seed=7):
    rng = np.random.default_rng(seed)
    # low-frequency colour field, plus fine detail (noise, stripes, a checker patch), tiled wider than the screen so a drift has something to bring in
    def blur(a, k):
        for _ in range(k):
            a = (a + np.roll(a, 1, 0) + np.roll(a, -1, 0) + np.roll(a, 1, 1) + np.roll(a, -1, 1)) / 5.0
        return a
    W, H = w + 64, h + 64
    base = np.stack([blur(rng.random((H, W)), 40) for _ in range(3)], -1)
    base = (base - base.min()) / (base.max() - base.min())
    fine = rng.random((H, W, 1)) * 0.25
    yy, xx = np.mgrid[0:H, 0:W]
    stripes = (((xx + yy) // 6) % 2)[..., None] * 0.15
    checker = ((((xx // 12) + (yy // 12)) % 2)[..., None] * 0.2) * ((xx > W // 2) & (yy < H // 2))[..., None]
    img = np.clip(base * 0.75 + fine + stripes + checker, 0, 1)
    return img.astype(np.float32)


def menu(w, h):
    """The panel's mask (1 where it covers) and its colour image: dark with light lines."""
    x0, x1, y0, y1 = int(w * 0.16), int(w * 0.68), int(h * 0.18), int(h * 0.74)
    col = np.zeros((h, w, 3), np.float32); mask = np.zeros((h, w, 1), np.float32)
    col[y0:y1, x0:x1] = (0.10, 0.11, 0.16); mask[y0:y1, x0:x1] = 1
    for k in range(12):   # lines of "text"
        y = y0 + 40 + k * ((y1 - y0 - 80) // 12)
        col[y:y + 6, x0 + 40:x1 - 40 - (k * 37) % 260] = (0.85, 0.80, 0.60)
    col[y0:y0 + 3, x0:x1] = col[y1 - 3:y1, x0:x1] = (0.9, 0.9, 0.9)
    return col, mask


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--w', type=int, default=1920); ap.add_argument('--h', type=int, default=1080)
    ap.add_argument('--frames', type=int, default=160)
    ap.add_argument('--on', type=int, default=40); ap.add_argument('--off', type=int, default=90)
    ap.add_argument('--drift', type=float, default=0.5, help='pixels a frame the scene moves sideways (0: still)')
    ap.add_argument('--alpha', type=float, default=0.85, help='how much the panel covers (1: opaque)')
    a = ap.parse_args()
    big = scene(a.w, a.h)
    mcol, mmask = menu(a.w, a.h)
    freq = 10_000_000
    DXGI_R8G8B8A8_UNORM = 28
    hdr = struct.pack('<8sIIIIIIII q I 3I 64s', b'LSREC01\0', 128, 1, a.w, a.h, DXGI_R8G8B8A8_UNORM, 4, a.frames, 2, freq, 0, 0, 0, 0, b'menuclip')
    assert len(hdr) == 128, len(hdr)
    with open(a.out, 'wb') as f:
        f.write(hdr)
        for i in range(a.frames):
            dx = int(i * a.drift)
            img = big[8:8 + a.h, 8 + dx % 48:8 + dx % 48 + a.w].copy()
            if a.on <= i < a.off:
                img = img * (1 - a.alpha * mmask) + mcol * (a.alpha * mmask)
            rgba = np.empty((a.h, a.w, 4), np.uint8)
            rgba[..., :3] = np.clip(img * 255 + 0.5, 0, 255).astype(np.uint8); rgba[..., 3] = 255
            raw = rgba.tobytes()
            f.write(struct.pack('<QqIIII', i, int(i * freq / 60), 0, len(raw), len(raw), 0))
            f.write(raw)
    print(f'{a.out}: {a.frames} frames {a.w}x{a.h}, the menu on at {a.on}, off at {a.off}')


if __name__ == '__main__':
    main()
