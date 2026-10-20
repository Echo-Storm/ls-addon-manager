"""check_grab.py GRAB.bmp [speed=8] [size=1280x720]: which scroll position does a picture grabbed from the dlssg_probe window show?

The probe draws a pattern that scrolls `speed` pixels a real frame, so the true picture for any scroll position s can be drawn here exactly. This finds the s that
fits the grabbed picture best and says how well: s a whole multiple of the speed is a real frame; half way between two is the generated in-between frame; the error
says how clean it is (0 = exact). A generated frame that is wrong (blurred, torn, smeared) has a larger error than either neighbour's."""
import sys, struct
import numpy as np

def load_bmp(path):
    d = open(path, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    px = np.frombuffer(d, np.uint8, count=abs(h) * w * 4, offset=off).reshape(abs(h), w, 4)
    if h > 0: px = px[::-1]
    return px[:, :, 2::-1].astype(np.float32) / 255.0   # RGB

def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)

def render(shift, w, h, x0=0, y0=0):
    y, x = np.mgrid[y0:y0 + h, x0:x0 + w].astype(np.float32)
    px = x + 0.5 + np.float32(shift); py = y + 0.5
    cx = np.floor(px / 16).astype(np.uint32); cy = np.floor(py / 16).astype(np.uint32)
    with np.errstate(over='ignore'):
        hh = cx * np.uint32(374761393) + cy * np.uint32(668265263)
        hh = (hh ^ (hh >> np.uint32(13))) * np.uint32(1274126177)
        hh = hh ^ (hh >> np.uint32(16))
    n = (hh & np.uint32(0xFFFF)).astype(np.float32) / 65535.0
    qx = (px / 128 - np.floor(px / 128)) - 0.5; qy = (py / 128 - np.floor(py / 128)) - 0.5
    ring = smoothstep(0.03, 0.0, np.abs(np.sqrt(qx * qx + qy * qy) - 0.3))
    return np.stack([n * 0.6 + ring * 0.4, n * 0.4 + np.abs(qx) * 0.5, n * 0.5 + np.abs(qy) * 0.5], -1)

def burst(path, speed):
    d = np.fromfile(path, np.uint8).reshape(-1, 192, 192, 4)
    span = int(speed * 400)
    P = 64                                  # a 64 x 64 patch in the middle is enough, and fast
    o = (192 - P) // 2
    # the pattern at every whole-pixel scroll position is this strip read at an offset (half pixels: a second strip drawn half a pixel on)
    strips = [render(h_, span + 192, 192, 300, 200)[o:o + P] for h_ in (0.0, 0.5)]
    rows = []
    for g in d:
        patch = (g[o:o + P, o:o + P, 2::-1].astype(np.float32) / 255.0)
        best = (9.0, 0.0)
        for h_, strip in zip((0.0, 0.5), strips):
            win = np.lib.stride_tricks.sliding_window_view(strip, (P, P, 3))[:, :, 0]   # (1, span+192-P+1, P, P, 3)
            for k in range(0, span, 1):
                e = float(np.abs(win[0, k + o] - patch).mean())
                if e < best[0]: best = (e, k + h_)
        rows.append(best)
    real = [r for r in rows if min((r[1] / speed) % 1, 1 - (r[1] / speed) % 1) < 0.1]
    mid = [r for r in rows if abs((r[1] / speed) % 1 - 0.5) < 0.15]
    print('%d grabs: %d real frames (mean error %.4f), %d in-between frames (mean error %.4f), %d neither' % (len(rows), len(real), np.mean([r[0] for r in real]) if real else -1, len(mid),
          np.mean([r[0] for r in mid]) if mid else -1, len(rows) - len(real) - len(mid)))
    print('worst few: ' + ', '.join('%.1fpx err %.3f' % (r[1], r[0]) for r in sorted(rows, reverse=True)[:5]))

def main():
    if sys.argv[1].endswith('.raw'):
        burst(sys.argv[1], float(dict(s.split('=') for s in sys.argv[2:]).get('speed', 8))); return
    a = {k: v for k, v in (s.split('=') for s in sys.argv[2:])}
    speed = float(a.get('speed', 8)); w, h = map(int, a.get('size', '1280x720').split('x'))
    img = load_bmp(sys.argv[1])
    X0, Y0, PW, PH = 300, 200, 192, 192   # a patch is enough to tell the scroll position, and is fast
    patch = img[Y0:Y0 + PH, X0:X0 + PW]
    def err(shift): return float(np.abs(render(shift, PW, PH, X0, Y0) - patch).mean())
    # whole pixels over a wide range (the probe has run a few hundred frames), then half pixels around the best
    errs = sorted((err(s_), float(s_)) for s_ in np.arange(0, int(speed * 400)))
    e0, s0 = errs[0]
    fine = sorted((err(s_), float(s_)) for s_ in np.arange(s0 - 4, s0 + 4.01, 0.5))
    e, s = fine[0]
    phase = (s / speed) % 1.0
    print('best scroll position %.1f px (error %.4f); a real frame is at multiples of %.1f, this is %.2f of the way between two' % (s, e, speed, phase))
    print('next best: ' + ', '.join('%.1f px (%.4f)' % (b, a_) for a_, b in fine[1:4]))
    kind = 'a real frame' if min(phase, 1 - phase) < 0.1 else ('an in-between frame' if abs(phase - 0.5) < 0.15 else 'neither')
    print('=> %s, %s' % (kind, 'clean (error < 0.01)' if e < 0.01 else 'not clean (error %.3f)' % e))

main()
