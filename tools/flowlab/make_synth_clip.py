"""make_synth_clip.py OUT_FOLDER [reals=14] [w=1280 h=720]: a test scene for the flow's hard cases, with the true frame between known exactly.

Writes OUT_FOLDER/frames.rgba (frame 2i is the real frame i, frame 2i+1 is the truth half way between real i and real i+1; RGBA8, the layout flowlab and the labs read) and masks.npz (where
each case is). The scene, moving at speeds that make the hard cases hard:
  background  a textured wall panning (14, 3) pixels a real frame
  periodic    a block of fine upright lines (period 7 px) on the wall's own motion (14, 3): 14 is two periods, so the lines alone match as well at no motion or one line on as where it truly went
  pole        a thin bright bar 5 px wide, in front of the wall, moving (-31, 0): too thin and fast for the smaller sizes of a pyramid, and faster than what is behind it
  plain       a soft gradient with a faint grain, moving with the wall: a block of it matches anywhere
Run `flowlab.exe rawframes=OUT_FOLDER/frames.rgba out=OUT_FOLDER fmt=gray scale=2` to make the flow, then the labs on OUT_FOLDER.
"""
import os
import sys
import numpy as np


def noise_texture(h, w, seed):
    rng = np.random.default_rng(seed)
    n = np.zeros((h, w), np.float32)
    for octave, amp in ((64, 0.5), (16, 0.3), (4, 0.2)):
        small = rng.random((h // octave + 3, w // octave + 3)).astype(np.float32)
        yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
        y0 = (yy / octave).astype(np.int32); x0 = (xx / octave).astype(np.int32)
        fy = yy / octave - y0; fx = xx / octave - x0
        n += amp * (small[y0, x0] * (1 - fx) * (1 - fy) + small[y0, x0 + 1] * fx * (1 - fy) + small[y0 + 1, x0] * (1 - fx) * fy + small[y0 + 1, x0 + 1] * fx * fy)
    return n


def sample(tex, xs, ys):
    h, w = tex.shape
    xs = np.mod(xs, w - 2); ys = np.mod(ys, h - 2)
    x0 = xs.astype(np.int32); y0 = ys.astype(np.int32); fx = xs - x0; fy = ys - y0
    return tex[y0, x0] * (1 - fx) * (1 - fy) + tex[y0, x0 + 1] * fx * (1 - fy) + tex[y0 + 1, x0] * (1 - fx) * fy + tex[y0 + 1, x0 + 1] * fx * fy


def main():
    out = sys.argv[1]
    args = {k: v for k, v in (s.split('=') for s in sys.argv[2:])}
    reals = int(args.get('reals', 14)); w = int(args.get('w', 1280)); h = int(args.get('h', 720))
    os.makedirs(out, exist_ok=True)
    wall = noise_texture(h * 2, w * 2, 1)
    wall2 = noise_texture(h * 2, w * 2, 2)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    # the cases' places
    def region(x0, x1, y0, y1, t):   # a rectangle of the wall's own coordinates, where it is on the screen at time t (real frames)
        bx, by = 14.0 * t, 3.0 * t
        return (xx + bx > x0) & (xx + bx < x1) & (yy + by > y0) & (yy + by < y1)
    pole_y = (yy > 380) & (yy < 640)
    per_t, pla_t, pole_t = [], [], []
    frames = []
    total = reals * 2 - 1
    for k in range(total):
        t = k / 2.0                                  # in real frames
        bx, by = 14.0 * t, 3.0 * t
        img = 0.25 + 0.5 * sample(wall, xx + bx, yy + by)
        img = img[..., None] * np.array([1.0, 0.95, 0.9], np.float32)
        periodic = region(700, 880, 120, 250, t); plain = region(1100, 1280, 120, 250, t)
        # periodic lines
        px = bx   # the face moves with the wall (a tower's face, a fence at the wall's depth): 14 px is two periods of the lines, so the lines alone cannot show the motion
        lines = 0.5 + 0.5 * np.sin(2 * np.pi * (xx - px) / 7.0)
        tint = 0.35 + 0.25 * sample(wall2, xx * 0.35 + 0.0, yy * 0.35 + 0.0)        # a slow variation on the face (fixed to the face's own frame: moves with the lines)
        tint = 0.35 + 0.25 * sample(wall2, (xx + px) * 0.35, (yy + by) * 0.35)   # a slow variation that moves with the wall: the only thing that shows the motion
        face = (lines * tint)[..., None] * np.array([0.9, 1.0, 1.0], np.float32) + 0.1
        img = np.where(periodic[..., None], face, img)
        # plain region: a gradient with a faint grain, moving with the wall
        grad = 0.45 + 0.2 * np.sin((xx + bx) / 220.0) * np.cos((yy + by) / 170.0) + 0.01 * sample(wall2, xx + bx, yy + by)
        img = np.where(plain[..., None], grad[..., None] * np.array([0.8, 0.9, 1.0], np.float32), img)
        # the pole: a bright bar 5 px wide moving left 31 px a real frame, wrapping across the screen
        cx = (w + 200 - 31.0 * t) % (w + 400) - 200
        bar = (np.abs(xx - cx) < 2.5) & pole_y
        img = np.where(bar[..., None], np.array([0.95, 0.92, 0.6], np.float32), img)
        if k % 2 == 0:
            pass
        else:
            per_t.append(periodic); pla_t.append(plain); pole_t.append((np.abs(xx - cx) < 40) & pole_y)
        frames.append((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8))
    rgba = np.concatenate([np.concatenate([f, np.full((h, w, 1), 255, np.uint8)], -1)[None] for f in frames])
    rgba.tofile(os.path.join(out, 'frames.rgba'))
    wall_t = [~(a | b | c) for a, b, c in zip(per_t, pla_t, pole_t)]
    np.savez(os.path.join(out, 'masks_t.npz'), periodic=np.array(per_t), plain=np.array(pla_t), pole=np.array(pole_t), wall=np.array(wall_t))
    print('wrote %d frames (%d real) to %s' % (total, reals, out))


if __name__ == '__main__':
    main()
