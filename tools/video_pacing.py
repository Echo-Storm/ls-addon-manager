"""Pacing of a recording of the screen (what a capture card or a recorder saw of Lossless Scaling's output): how many of its frames are new pictures, how long each picture stayed,
how even that is, and where it stuttered.

    python tools/video_pacing.py <video> [--ffmpeg <path>] [--thr 0.4] [--csv out.csv] [--cuda]

Each frame is shrunk to 480x270 grey (on the card with --cuda, which is much faster for 4K HEVC) and compared with the one before: the mean change in levels of 255. A change under
--thr (0.4 by default; a lossy recording of an unchanged picture stays well under it) is a repeat of the picture before. Reported: the unique pictures a second (overall and in
5 s windows), the run lengths of each picture in frames of the recording (a steady x2 from 60 is all 1s at 120 fps; 60 fps from a game with no generation is all 2s), the
share of runs that are not the usual one, the longest freezes with their times, and the evenness of the gaps between new pictures (the coefficient of variation of the run lengths).
"""
import argparse
import os
import subprocess
import sys

import numpy as np

W, H = 480, 270


def probe(ffprobe, path):
    out = subprocess.run([ffprobe, '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=width,height,r_frame_rate,nb_frames:format=duration', '-of', 'default=nw=1', path],
                         capture_output=True, text=True).stdout
    info = dict(line.split('=', 1) for line in out.strip().splitlines() if '=' in line)

    def number(key, kind):   # ffprobe prints N/A for what a container does not carry (MKV has no frame count)
        try:
            return kind(info.get(key, 0))
        except ValueError:
            return kind(0)
    fps = 0.0
    try:
        num, den = info.get('r_frame_rate', '0/0').split('/')
        fps = float(num) / float(den)
    except (ValueError, ZeroDivisionError):
        pass
    if fps <= 0:
        print('WARNING: ffprobe did not give the frame rate (is --ffmpeg next to an ffprobe?); 60 fps is assumed and every time and rate below is wrong if it is not')
        fps = 60.0
    return fps, number('duration', float), number('nb_frames', int)


def diffs(ffmpeg, path, cuda):
    if cuda:
        vf = 'scale_cuda=%d:%d:format=nv12,hwdownload,format=nv12,format=gray' % (W, H)
        cmd = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-hwaccel', 'cuda', '-hwaccel_output_format', 'cuda', '-i', path, '-vf', vf, '-f', 'rawvideo', '-pix_fmt', 'gray', '-']
    else:
        cmd = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-i', path, '-vf', 'scale=%d:%d,format=gray' % (W, H), '-f', 'rawvideo', '-pix_fmt', 'gray', '-']
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, bufsize=1 << 24)
    size = W * H
    prev = None
    d = []
    luma = []
    while True:
        buf = p.stdout.read(size * 64)
        if not buf:
            break
        n = len(buf) // size
        frames = np.frombuffer(buf, np.uint8, count=n * size).reshape(n, size).astype(np.int16)
        if prev is not None:
            d.append(np.abs(frames[0] - prev).mean())
        if n > 1:
            d.extend(np.abs(frames[1:] - frames[:-1]).mean(axis=1))
        luma.extend(frames.mean(axis=1))
        prev = frames[-1]
        if len(buf) % size:
            break
    p.stdout.close()
    p.wait()
    if p.returncode != 0 or not d:
        sys.exit('ffmpeg failed (exit %s) or gave fewer than two frames: nothing to measure (run it by hand to see why%s)' % (p.returncode, '; --cuda needs an NVIDIA card and a build with cuda' if cuda else ''))
    return np.array(d, np.float64), np.array(luma, np.float64)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('video')
    ap.add_argument('--ffmpeg', default=r'D:\Applications\MPV\ffmpeg.exe')
    ap.add_argument('--thr', type=float, default=0.4)
    ap.add_argument('--csv', default='')
    ap.add_argument('--cuda', action='store_true')
    ap.add_argument('--active', type=int, default=30, help='new pictures in a second for it to count as moving')
    args = ap.parse_args()
    head, tail = os.path.split(args.ffmpeg)
    ffprobe = os.path.join(head, tail.lower().replace('ffmpeg', 'ffprobe')) if 'ffmpeg' in tail.lower() else 'ffprobe'
    fps, duration, nb = probe(ffprobe, args.video)
    d, luma = diffs(args.ffmpeg, args.video, args.cuda)
    n = len(d) + 1
    new = np.concatenate([[True], d >= args.thr])   # the first frame is a picture; a frame is new when it changed
    idx = np.flatnonzero(new)
    runs = np.diff(np.concatenate([idx, [n]]))   # frames each picture stayed
    t = lambda i: i / fps
    print('%s' % args.video)
    print('  %d frames at %.0f fps (%.1f s); %d new pictures = %.1f a second (%.1f%% of the frames are repeats)' % (n, fps, n / fps, len(idx), len(idx) / (n / fps), 100.0 * (1 - len(idx) / n)))
    hist = {k: int((runs == k).sum()) for k in range(1, 6)}
    hist['6+'] = int((runs >= 6).sum())
    print('  how long each picture stayed (frames of the recording): ' + ', '.join('%s: %d' % (k, v) for k, v in hist.items()))
    mode = int(np.bincount(runs).argmax())
    odd = float((runs != mode).mean())
    print('  the usual stay is %d frame(s); %.1f%% of the pictures stayed another length; evenness (coefficient of variation of the stays) %.2f' % (mode, 100 * odd, runs.std() / max(runs.mean(), 1e-9)))
    # windows of 5 s
    win = int(round(fps * 5))
    rows = []
    for a in range(0, n - win // 2, win):
        b = min(n, a + win)
        c = int(new[a:b].sum())
        rows.append((t(a), c / ((b - a) / fps)))
    print('  new pictures a second in 5 s windows: ' + ' '.join('%.0f' % r[1] for r in rows[:60]) + (' ...' if len(rows) > 60 else ''))
    worst = np.argsort(-runs)[:8]
    print('  the longest stays: ' + '; '.join('%d frames (%.0f ms) at %.1f s' % (runs[k], 1000.0 * runs[k] / fps, t(idx[k])) for k in sorted(worst, key=lambda k: -runs[k])))
    # big jumps (a picture much further from the one before than usual): stutter shows as a long stay then a jump
    jump = d[d >= args.thr]
    if len(jump):
        print('  the change from one new picture to the next: median %.2f, p95 %.2f, max %.2f levels' % (np.median(jump), np.percentile(jump, 95), jump.max()))
    # a repeating pattern in the stays
    if len(runs) > 40:
        r = runs - runs.mean(); var = (r * r).sum()
        best = max(((float((r[:-lag] * r[lag:]).sum() / var), lag) for lag in range(2, 13)), default=(0, 0)) if var > 0 else (0, 0)
        print('  repeating pattern in the stays: %s' % ('every %d pictures (correlation %.2f)' % (best[1], best[0]) if best[0] > 0.35 else 'none (best %.2f at %d)' % best))
    # the moving parts only: a still scene repeats its picture for as long as nothing moves, which says nothing about pacing. Seconds with at least --active new pictures are
    # looked at on their own: the stays inside them, the share of stays of 3 frames or more (a stutter at 120 fps is 25 ms), and each second's unevenness
    sec = int(n // fps)
    act = []
    for k in range(sec):
        a0, b0 = int(k * fps), int((k + 1) * fps)
        w_idx = np.flatnonzero(new[a0:b0]) + a0
        if len(w_idx) >= args.active:
            act.append((k, w_idx))
    if act:
        stays = []
        per = []
        for k, w_idx in act:
            st = np.diff(np.concatenate([w_idx, [int((k + 1) * fps)]]))
            stays.extend(st.tolist())
            # unevenness: how far the pictures are from evenly spread across the second (the biggest gap against the average gap)
            per.append((k, len(w_idx), float(st.max()), float((st >= 3).sum()), float(np.abs(st - st.mean()).mean())))
        stays = np.array(stays)
        print('  MOVING seconds: %d (%.0f%% of the recording), %.1f new pictures a second there' % (len(act), 100.0 * len(act) / max(sec, 1), np.mean([len(w) for _, w in act])))
        print('  in them the stays: 1: %.0f%%, 2: %.0f%%, 3: %.0f%%, 4+: %.0f%%; the biggest stay %d frames (%.0f ms)' % tuple([100.0 * (stays == 1).mean(), 100.0 * (stays == 2).mean(), 100.0 * (stays == 3).mean(), 100.0 * (stays >= 4).mean(), int(stays.max()), 1000.0 * stays.max() / fps]))
        rate = np.array([r[1] for r in per])
        print('  new pictures a second in the moving seconds: min %d, p10 %d, median %d, p90 %d, max %d' % (rate.min(), np.percentile(rate, 10), np.median(rate), np.percentile(rate, 90), rate.max()))
        un = np.array([r[4] for r in per])
        print('  unevenness of the stays in a second (mean distance from the average stay of the second, frames): median %.2f, p90 %.2f, worst %.2f' % (np.median(un), np.percentile(un, 90), un.max()))
        worst = sorted(per, key=lambda r: (-r[3], -r[2]))[:6]
        print('  the roughest moving seconds (at s: pictures, longest stay in frames, stays of 3+): ' + '; '.join('%ds: %d, %d, %d' % (r[0], r[1], r[2], r[3]) for r in worst))
    # a picture whose frames differ little (a slow pan, low contrast) puts many real changes just above the threshold, and some under it: the counts above are then unreliable
    near = float(((jump >= args.thr) & (jump < 2 * args.thr)).mean()) if len(jump) else 0.0
    if near > 0.15:
        print('  NOTE: %.0f%% of the changes between new pictures are under %.2f levels (twice --thr): a low-contrast or slow scene; repeats and new pictures cannot be told apart reliably, so the numbers above may be wrong (try a lower --thr)' % (100 * near, 2 * args.thr))
    # the noise floor: how far apart the repeats and the new pictures are
    rep = d[d < args.thr]
    print('  repeats differ by %.3f levels on average (p99 %.3f); new pictures by %.2f; mean luma %.1f' % (rep.mean() if len(rep) else 0, np.percentile(rep, 99) if len(rep) else 0, jump.mean() if len(jump) else 0, luma.mean()))
    if args.csv:
        with open(args.csv, 'w') as f:
            f.write('frame,time_s,change,new,luma\n')
            for i in range(n):
                f.write('%d,%.4f,%.3f,%d,%.2f\n' % (i, t(i), 0 if i == 0 else d[i - 1], int(new[i]), luma[i]))


if __name__ == '__main__':
    main()
