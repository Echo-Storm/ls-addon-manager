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

NMAX = 240

def luma(a):
    return a[..., 0] * 0.3 + a[..., 1] * 0.59 + a[..., 2] * 0.11

def burst(path, speed):
    """The desktop picture is brighter than what was drawn (the compositor maps the picture to the screen's white level), so a picture is scored by its correlation with the
    drawn pattern, which does not care about brightness and contrast: error = 1 - correlation (0 = the same picture)."""
    d = np.fromfile(path, np.uint8).reshape(-1, 192, 192, 4)[:NMAX]
    span = int(speed * 400)
    P = 96
    o = (192 - P) // 2
    strips = [luma(render(h_, span + 192, 192, 300, 200))[o:o + P] for h_ in (0.0, 0.5)]
    wins = [np.lib.stride_tricks.sliding_window_view(st, (P, P))[0] for st in strips]          # (positions, P, P)
    wins = [w - w.mean(axis=(1, 2), keepdims=True) for w in wins]
    norms = [np.sqrt((w * w).sum(axis=(1, 2))) + 1e-9 for w in wins]
    rows = []
    for g in d:
        patch = luma(g[o:o + P, o:o + P, 2::-1].astype(np.float32) / 255.0)
        patch = patch - patch.mean(); pn = np.sqrt((patch * patch).sum()) + 1e-9
        best = (9.0, 0.0)
        for h_, w, nrm in zip((0.0, 0.5), wins, norms):
            ncc = np.tensordot(w[o:o + span], patch, axes=([1, 2], [0, 1])) / (nrm[o:o + span] * pn)
            k = int(np.argmax(ncc))
            if 1 - float(ncc[k]) < best[0]: best = (1 - float(ncc[k]), k + h_)
        rows.append(best)
    ph = lambda r: (r[1] / speed) % 1
    real = [r for r in rows if min(ph(r), 1 - ph(r)) < 0.1]
    mid = [r for r in rows if abs(ph(r) - 0.5) < 0.15]
    other = len(rows) - len(real) - len(mid)
    print('%d pictures: %d at a real frame (mean error %.4f), %d at an in-between position (mean error %.4f), %d at neither' % (len(rows), len(real), np.mean([r[0] for r in real]) if real else -1,
          len(mid), np.mean([r[0] for r in mid]) if mid else -1, other))
    errs = sorted(r[0] for r in rows)
    print('all pictures: error median %.4f, worst %.4f' % (errs[len(errs) // 2], errs[-1]))
    print('in order (px): ' + ' '.join('%g' % r[1] for r in rows[:40]))

def main():
    global NMAX
    NMAX = int(dict(s.split('=') for s in sys.argv[2:]).get('n', 240))
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
