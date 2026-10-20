"""check_clip.py CAPTURE.raw [w=1280 h=720]: how good are DLSS-G's generated frames on a recorded clip, against the true in-between frames?

dlssg_probe.exe clip=FILE.lsrec grab=CAPTURE.raw writes CAPTURE.raw (the desktop pictures of the window, BGRA, one after the other) and CAPTURE.raw.clip (the clip's frames,
cropped to the window, RGBA: frame 2i is shown as a real frame, frame 2i+1 is what lies between real frames i and i+1: the truth for the frame generated there).

For each picture of the screen: is it a real frame (it matches one of the even clip frames exactly), or a generated one? A generated one is scored against the odd clip frame it
is closest to, and the same frame is scored against two things that need no model: the two real frames around it blended half and half, and the earlier real frame repeated.
The score is the correlation of the brightness (the desktop shows the picture at the screen's white level, brighter than what was drawn, so a score that ignores brightness and
contrast is the honest one): 1.0 = the same picture.
"""
import sys
import numpy as np


def luma(a):
    return a[..., 2].astype(np.float32) * 0.299 + a[..., 1].astype(np.float32) * 0.587 + a[..., 0].astype(np.float32) * 0.114   # BGRA


def lumaRGBA(a):
    return a[..., 0].astype(np.float32) * 0.299 + a[..., 1].astype(np.float32) * 0.587 + a[..., 2].astype(np.float32) * 0.114


def ds(a, k=4):
    h, w = a.shape
    return a[:h // k * k, :w // k * k].reshape(h // k, k, w // k, k).mean(axis=(1, 3))


def ncc(a, b):
    a = a - a.mean(); b = b - b.mean()
    d = np.sqrt((a * a).sum() * (b * b).sum()) + 1e-9
    return float((a * b).sum() / d)


def main():
    args = {k: v for k, v in (s.split('=') for s in sys.argv[2:])}
    w = int(args.get('w', 1280)); h = int(args.get('h', 720))
    cap = np.memmap(sys.argv[1], np.uint8, 'r').reshape(-1, h, w, 4)
    clip = np.memmap(sys.argv[1] + '.clip', np.uint8, 'r').reshape(-1, h, w, 4)
    n = len(clip) // 2 * 2
    cl = [lumaRGBA(clip[i]) for i in range(n)]
    cs = np.stack([ds(x) for x in cl])
    csn = cs - cs.mean(axis=(1, 2), keepdims=True)
    csn /= (np.sqrt((csn * csn).sum(axis=(1, 2)))[:, None, None] + 1e-9)
    # which pictures are real frames (they match an even clip frame best) and which are generated (they match an odd one best); a picture the compositor showed twice counts once
    pics = []
    prev = None
    for k in range(len(cap)):
        p = luma(cap[k]); pd = ds(p)
        if prev is not None and np.abs(pd - prev).max() < 0.5: continue
        prev = pd
        q = pd - pd.mean(); q /= (np.sqrt((q * q).sum()) + 1e-9)
        scores = (csn * q).sum(axis=(1, 2))
        pics.append((k, p, scores))
    real = [x for x in pics if x[2][0::2].max() > x[2][1::2].max() and x[2][0::2].max() > 0.95]
    gen = [x for x in pics if x not in real]
    # the screen shows the picture at its own brightness: learn the mapping from the real frames (drawn -> shown), a cubic on the brightness, and put the baselines through it too
    xs = []; ys = []
    for k, p, scores in real[:12]:
        e = 2 * int(np.argmax(scores[0::2]))
        xs.append(cl[e][::7, ::7].ravel()); ys.append(p[::7, ::7].ravel())
    coef = np.polyfit(np.concatenate(xs), np.concatenate(ys), 3)
    show = lambda a: np.polyval(coef, a)
    rows = []
    for k, p, scores in gen:
        odd = scores[1::2]; j = 2 * int(np.argmax(odd)) + 1
        if j + 1 >= n: continue                       # the last truth frame has no real frame after it
        t = show(cl[j])
        rows.append((k, j, ncc(p, t), ncc(show(0.5 * (cl[j - 1] + cl[j + 1])), t), ncc(show(cl[j - 1]), t), ncc(show(cl[j + 1]), t)))
    rr = [ncc(p, show(cl[2 * int(np.argmax(s[0::2]))])) for k, p, s in real[:40]]
    print('%d different pictures: %d real frames (they match the clip at %.4f, the ceiling of this measurement), %d generated (scored)' % (len(pics), len(real), np.mean(rr) if rr else 0, len(rows)))
    if not rows: return
    r = np.array([x[2:] for x in rows])
    names = ['DLSS-G generated', 'real frames blended', 'earlier real frame repeated', 'later real frame repeated']
    for i, nme in enumerate(names):
        print('  %-28s correlation with the true in-between frame: mean %.4f  median %.4f  worst %.4f' % (nme, r[:, i].mean(), np.median(r[:, i]), r[:, i].min()))
    print('  generated beats the blend in %d of %d pictures' % (int((r[:, 0] > r[:, 1]).sum()), len(r)))


main()
