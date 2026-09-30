#!/usr/bin/env python
"""Offline lab for the temporal filters on Neural Rendering's change (the model's "delta"), on real data.

  nr_nreval.exe <recording.lsrec> <folder> first=60 count=80 scale=30 smooth=0 dump=<file> lsdir=<folder with nvngx_dlssnr.dll>
  python tools/nr_filter_lab.py <file> [stride=3]

The dump holds, for every frame after the first, the still-pixel mask (1 where the game's frame did not change from the frame before) and the model's own change (smooth=0),
at the working size. Each candidate filter is run over every pixel's time series (numpy, all pixels at once); the score is the flicker left in the filtered change where the
game stood still (the mean of max over channels of |out(t) - out(t-1)|, levels of 255, over pixels still in both frames): the number `nr_nreval` calls "still".
The response is measured on a clean step of 0.05 (12.75 levels): the frames until the filter's output has come 90 % of the way. A filter is only better than another at the same
response: lower flicker for the same or a shorter time.
"""
import sys
import numpy as np

def load(path, stride):
    raw = open(path, 'rb').read()
    dw, dh = np.frombuffer(raw[:8], dtype=np.uint32)
    n = dw * dh
    rec = n + n * 3 * 2
    frames = (len(raw) - 8) // rec
    masks, deltas = [], []
    for f in range(frames):
        o = 8 + f * rec
        m = np.frombuffer(raw[o:o + n], dtype=np.uint8).reshape(dh, dw)
        d = np.frombuffer(raw[o + n:o + rec], dtype=np.float16).reshape(dh, dw, 3).astype(np.float32)
        masks.append(m[::stride, ::stride].copy())
        deltas.append(d[::stride, ::stride].copy())
    return np.array(masks), np.array(deltas)

# ---- the filters: each takes the series (T, H, W, 3) and the still masks (T, H, W) and returns the filtered series. "history is kept while a pixel is still" for all of them.
def f_raw(x, m, **k): return x.copy()

def f_ema(x, m, w=0.95, **k):
    out = np.empty_like(x); h = x[0].copy(); out[0] = h
    for t in range(1, len(x)):
        keep = (m[t] > 0)[..., None]
        h = np.where(keep, w * h + (1 - w) * x[t], x[t])
        out[t] = h
    return out

def f_dual(x, m, wf=0.5, ws=0.95, thr=0.02, k=2, **kw):
    # a fast and a slow average; the slow one is shown until the fast one has disagreed with it by more than thr for k frames in a row: then the slow one is reset to the fast
    out = np.empty_like(x); hf = x[0].copy(); hs = x[0].copy(); run = np.zeros(x.shape[1:3], dtype=np.int32); out[0] = hs
    for t in range(1, len(x)):
        still = (m[t] > 0)[..., None]
        hf = np.where(still, wf * hf + (1 - wf) * x[t], x[t])
        hs = np.where(still, ws * hs + (1 - ws) * x[t], x[t])
        dis = np.max(np.abs(hf - hs), axis=-1) > thr
        run = np.where(dis, run + 1, 0)
        reset = (run >= k)[..., None]
        hs = np.where(reset, hf, hs)
        run = np.where(run >= k, 0, run)
        out[t] = hs
    return out

def f_dual_soft(x, m, wf=0.6, ws=0.97, thr=0.015, **kw):
    # no counter: the more the fast average disagrees with the slow one (past thr), the more the slow one is pulled to the fast one this frame
    out = np.empty_like(x); hf = x[0].copy(); hs = x[0].copy(); out[0] = hs
    for t in range(1, len(x)):
        still = (m[t] > 0)[..., None]
        hf = np.where(still, wf * hf + (1 - wf) * x[t], x[t])
        dis = np.max(np.abs(hf - hs), axis=-1)[..., None]
        conf = np.clip((dis - thr) / thr, 0, 1); conf = conf * conf * (3 - 2 * conf)
        slow = ws * hs + (1 - ws) * x[t]
        hs = np.where(still, (1 - conf) * slow + conf * hf, x[t])
        out[t] = hs
    return out

def f_robust(x, m, whi=0.97, wlo=0.5, thr=0.02, **kw):
    # no extra state: the history's weight falls from whi to wlo as the new value gets far from it (|new - history| from thr to 3 thr): small differences are noise and are averaged hard, a large one is a real change and is taken
    out = np.empty_like(x); h = x[0].copy(); out[0] = h
    for t in range(1, len(x)):
        still = (m[t] > 0)[..., None]
        inn = np.max(np.abs(x[t] - h), axis=-1)[..., None]
        u = np.clip((inn - thr) / (2 * thr), 0, 1); u = u * u * (3 - 2 * u)
        w = whi + (wlo - whi) * u
        h = np.where(still, w * h + (1 - w) * x[t], x[t]); out[t] = h
    return out

def f_persist(x, m, dt=0.029, tau80=0.08, tup=0.06, tdown=0.18, lo=0.005, hi=0.02, **kw):
    # the change's amplitude A and its presence P kept apart; shown: P * A. Appear fast (tup), leave slowly (tdown); the amplitude follows the observation at the EMA's pace
    out = np.empty_like(x); A = x[0].copy(); P = np.ones(x.shape[1:3], dtype=np.float32); out[0] = A
    w80 = np.exp(-dt / tau80)
    for t in range(1, len(x)):
        still = (m[t] > 0)
        s = np.clip((np.max(np.abs(x[t]), axis=-1) - lo) / (hi - lo), 0, 1); s = s * s * (3 - 2 * s)   # smoothstep(lo, hi, |x|)
        wsup = np.where(s > P, np.exp(-dt / tup), np.exp(-dt / tdown))
        Pn = wsup * P + (1 - wsup) * s
        An = A * w80 + x[t] * (1 - w80)
        A = np.where(still[..., None], An, x[t]); P = np.where(still, Pn, s)
        out[t] = P[..., None] * A
    return out

def f_adaptive(x, m, whi=0.97, wlo=0.5, z0=2.0, **kw):
    # the weight of the history follows how surprising the new value is, against the pixel's own noise (a running mean of |new - history|): a value within the noise is averaged hard, one far outside it is taken
    out = np.empty_like(x); h = x[0].copy(); sig = np.full(x.shape[1:3], 0.01, dtype=np.float32); out[0] = h
    for t in range(1, len(x)):
        still = (m[t] > 0)[..., None]
        inn = np.max(np.abs(x[t] - h), axis=-1)
        z = inn / (sig + 1e-4)
        w = np.where(z < z0, whi, wlo + (whi - wlo) * np.clip(1 - (z - z0) / z0, 0, 1))[..., None]
        hn = w * h + (1 - w) * x[t]
        sig = np.where(m[t] > 0, 0.9 * sig + 0.1 * inn, sig)
        h = np.where(still, hn, x[t]); out[t] = h
    return out

def f_median3_ema(x, m, w=0.9, **kw):
    # a median of the last three values first (drops a single-frame flip), then the EMA
    med = x.copy()
    for t in range(2, len(x)):
        med[t] = np.median(np.stack([x[t], x[t - 1], x[t - 2]]), axis=0)
    return f_ema(med, m, w=w)

def still_flicker(out, m):
    sel = (m[1:] > 0) & (m[:-1] > 0)
    d = np.max(np.abs(out[1:] - out[:-1]), axis=-1)
    return float(d[sel].mean() * 255.0)

def response(filt, shape, amp=0.05, hold=12, total=100, **kw):
    # a clean step of `amp` in all channels, the pixels all still: frames until the output is 90 % of the way
    x = np.zeros((total,) + shape + (3,), dtype=np.float32); x[hold:] = amp
    m = np.ones((total,) + shape, dtype=np.uint8)
    y = filt(x, m, **kw)[:, 0, 0, 0]
    for t in range(hold, total):
        if y[t] >= 0.9 * amp: return t - hold
    return total - hold

def main():
    path = sys.argv[1]
    stride = 3
    for a in sys.argv[2:]:
        if a.startswith('stride='): stride = int(a.split('=')[1])
    m, x = load(path, stride)
    print('frames %d, pixels %dx%d (stride %d), still in %.0f %% of them' % (len(x), x.shape[2], x.shape[1], stride, 100.0 * (m > 0).mean()))
    cases = [('raw (no filter)', f_raw, {}),
             ('EMA 0.50', f_ema, dict(w=0.5)), ('EMA 0.80', f_ema, dict(w=0.8)), ('EMA 0.90', f_ema, dict(w=0.9)), ('EMA 0.95 (today at 0.8)', f_ema, dict(w=0.95)), ('EMA 0.97', f_ema, dict(w=0.97)),
             ('dual 0.5/0.95, reset after 2', f_dual, dict()), ('dual 0.5/0.97, reset after 2', f_dual, dict(ws=0.97)), ('dual 0.6/0.97, reset after 3', f_dual, dict(wf=0.6, ws=0.97, k=3)),
             ('dual soft 0.6/0.97 thr .015', f_dual_soft, dict()), ('dual soft 0.6/0.98 thr .015', f_dual_soft, dict(ws=0.98)), ('dual soft 0.7/0.97 thr .01', f_dual_soft, dict(wf=0.7, thr=0.01)),
             ('dual soft 0.5/0.97 thr .02', f_dual_soft, dict(wf=0.5, thr=0.02)), ('dual 0.6/0.98, reset after 3', f_dual, dict(wf=0.6, ws=0.98, k=3)), ('dual 0.7/0.97, reset after 3 thr .01', f_dual, dict(wf=0.7, ws=0.97, k=3, thr=0.01)),
             ('dual 0.6/0.97, reset after 4', f_dual, dict(wf=0.6, ws=0.97, k=4)), ('dual 0.6/0.97 thr .03 reset 3', f_dual, dict(wf=0.6, ws=0.97, k=3, thr=0.03)),
             ('robust 0.97/0.5 thr .02', f_robust, dict()), ('robust 0.97/0.5 thr .015', f_robust, dict(thr=0.015)), ('robust 0.97/0.5 thr .03', f_robust, dict(thr=0.03)), ('robust 0.98/0.5 thr .02', f_robust, dict(whi=0.98)),
             ('robust 0.96/0.5 thr .02', f_robust, dict(whi=0.96)), ('robust 0.98/0.6 thr .03', f_robust, dict(whi=0.98, wlo=0.6, thr=0.03)), ('robust 0.97/0.7 thr .02', f_robust, dict(wlo=0.7)),
             ('persistence (F) 60/180 ms', f_persist, dict()), ('persistence (F) 30/300 ms', f_persist, dict(tup=0.03, tdown=0.3)),
             ('adaptive 0.97/0.5', f_adaptive, dict()), ('adaptive 0.98/0.6', f_adaptive, dict(whi=0.98, wlo=0.6)), ('adaptive 0.95/0.5', f_adaptive, dict(whi=0.95)),
             ('median3 + EMA 0.9', f_median3_ema, dict())]
    print('%-34s %10s   frames to 90 %% of a step of 0.02 / 0.05 / 0.10' % ('filter', 'flicker'))
    base = None
    for name, f, kw in cases:
        out = f(x, m, **kw)
        fl = still_flicker(out, m)
        if base is None: base = fl
        rsp = [response(f, (2, 2), amp=a, **kw) for a in (0.02, 0.05, 0.10)]
        print('%-34s %7.3f %3.0f%%   %4d %4d %4d' % (name, fl, 100.0 * fl / base, rsp[0], rsp[1], rsp[2]), flush=True)

main()
