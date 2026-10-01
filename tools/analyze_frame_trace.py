#!/usr/bin/env python
"""Reads a frame trace (logs/frame-trace-<addon>.csv, written by the addons at shutdown and whenever a recording is saved) and prints a report: the pacing of the
presented frames, of the real frames, what the model and the upscaler did, and what happened around the longest gaps. Standard library only.

  python tools/analyze_frame_trace.py <trace.csv> [--out report.md] [--gaps 15] [--window 60]

The events (src/addon/frame_trace.h): tap (a real frame arrived), present (every present; a = the swap chain's number, the busiest is Lossless Scaling's output),
model (a model run ended: a = 1 started / 0 left out, b = its GPU start delay and c = its run, in 0.01 ms), upscale (an upscaler pass: a = 1 its picture was used,
b = the engine's GPU ms, c = its motion estimate's ms, in 0.01 ms), auto (auto quality: a = the model resolution in percent, b = the model runs on every Nth frame),
hotkey (a = which). Times are microseconds from the first event.
"""
import argparse, collections, csv, statistics, sys

def pct(sorted_vals, p):
    if not sorted_vals: return float('nan')
    i = min(len(sorted_vals) - 1, max(0, int(round((p / 100.0) * (len(sorted_vals) - 1)))))
    return sorted_vals[i]

def dist(vals):
    v = sorted(vals)
    if not v: return 'none'
    return 'n %d, mean %.2f, p50 %.2f, p95 %.2f, p99 %.2f, max %.2f' % (len(v), statistics.fmean(v), pct(v, 50), pct(v, 95), pct(v, 99), v[-1])

def low1_fps(intervals_ms):
    v = sorted(intervals_ms, reverse=True)
    k = max(1, len(v) // 100)
    worst = statistics.fmean(v[:k])
    return 1000.0 / worst if worst > 0 else float('nan')

def load(path):
    events, meta = [], {}
    with open(path, newline='') as f:
        for row in csv.reader(f):
            if not row: continue
            if row[0].startswith('#'):
                meta[row[0]] = row[1:]
                continue
            if row[0] == 't_us': continue
            events.append((int(row[0]), row[1], int(row[2]), int(row[3]), int(row[4])))
    events.sort(key=lambda e: e[0])
    return events, meta

def report(events, meta, gaps, window_ms):
    out = []
    w = out.append
    if not events:
        w('# Frame trace\n\nNo events.')
        return '\n'.join(out)
    span = (events[-1][0] - events[0][0]) / 1e6
    kinds = collections.Counter(e[1] for e in events)
    w('# Frame trace report')
    w('')
    w('%d events over %.1f s (%s).' % (len(events), span, ', '.join('%s %d' % (k, n) for k, n in sorted(kinds.items()))))
    if '#events' in meta: w('The trace had seen %s events; the newest %s are kept.' % (meta['#events'][0], meta['#events'][1].replace('kept ', '')))
    w('')

    # presents: the busiest swap chain is Lossless Scaling's output
    presents = [e for e in events if e[1] == 'present']
    chains = collections.Counter(e[2] for e in presents)
    long_gaps = []
    if presents:
        chain = chains.most_common(1)[0][0]
        pts = [e[0] for e in presents if e[2] == chain]
        iv = [(pts[i] - pts[i - 1]) / 1000.0 for i in range(1, len(pts))]
        w('## Presented frames (swap chain %d, %d of %d presents)' % (chain, len(pts), len(presents)))
        w('')
        w('- interval (ms): ' + dist(iv))
        if iv:
            med = statistics.median(iv)
            w('- fps from the median interval %.1f; from the slowest 1%% of intervals (low 1%%) %.1f' % (1000.0 / med, low1_fps(iv)))
            for mult in (1.5, 2, 3):
                n = sum(1 for x in iv if x > mult * med)
                w('- intervals over %.1fx the median (%.1f ms): %d (%.2f%%)' % (mult, mult * med, n, 100.0 * n / len(iv)))
            order = sorted(range(len(iv)), key=lambda i: -iv[i])[:gaps]
            long_gaps = [(pts[i + 1], iv[i]) for i in order]
            w('')
            w('The %d longest gaps (seconds into the trace, gap in ms, and what else happened within %d ms before it ended):' % (len(long_gaps), window_ms))
            w('')
            for t_end, g in sorted(long_gaps):
                near = [e for e in events if t_end - window_ms * 1000 <= e[0] <= t_end and e[1] not in ('present',)]
                what = ', '.join('%s%s' % (e[1], '(a=%d,b=%d,c=%d)' % (e[2], e[3], e[4]) if e[1] in ('model', 'auto', 'hotkey', 'upscale') else '') for e in near[:8])
                w('- %.3f s: %.1f ms  %s' % (t_end / 1e6, g, what or '(nothing else)'))
            # do the long gaps come at a regular spacing?
            ts = sorted(t for t, _ in long_gaps)
            if len(ts) > 3:
                d = [(ts[i] - ts[i - 1]) / 1e6 for i in range(1, len(ts))]
                w('')
                w('Spacing between the longest gaps (s): ' + ', '.join('%.2f' % x for x in d[:12]) + ('  (a steady spacing would point at a periodic cause)' if d and statistics.pstdev(d) < 0.2 * statistics.fmean(d) else ''))
        w('')

    # real frames
    taps = [e[0] for e in events if e[1] == 'tap']
    if len(taps) > 2:
        iv = [(taps[i] - taps[i - 1]) / 1000.0 for i in range(1, len(taps))]
        w('## Real frames (the tap)')
        w('')
        w('- interval (ms): ' + dist(iv))
        w('- fps from the median %.1f; low 1%% %.1f' % (1000.0 / statistics.median(iv), low1_fps(iv)))
        w('')

    # the model
    model = [e for e in events if e[1] == 'model']
    if model:
        started = [e for e in model if e[2] == 1]
        w('## Neural Rendering model')
        w('')
        w('- runs %d, left out (the model was busy) %d (%.1f%%)' % (len(started), len(model) - len(started), 100.0 * (len(model) - len(started)) / len(model)))
        if started:
            w('- GPU start delay (ms, how long it waited for the card): ' + dist([e[3] / 100.0 for e in started]))
            w('- run (ms): ' + dist([e[4] / 100.0 for e in started]))
        if long_gaps and started:
            overlap = 0
            for t_end, g in long_gaps:
                if any(t_end - (g * 1000.0) - 5000 <= e[0] <= t_end + 5000 for e in started): overlap += 1
            w('- of the %d longest present gaps, %d had a model run end inside them or within 5 ms: %s' % (len(long_gaps), overlap,
              'the model is a likely part of the gaps' if overlap >= 0.7 * len(long_gaps) else 'the model is not obviously the cause'))
        w('')

    # the upscaler
    up = [e for e in events if e[1] == 'upscale']
    if up:
        used = [e for e in up if e[2] & 1]
        light = [e for e in up if e[2] & 2]
        w('## Upscaler passes')
        w('')
        w('- passes %d, the upscaler\'s picture used on %d (%.1f%%)' % (len(up), len(used), 100.0 * len(used) / len(up)))
        if light: w('- generated frames given the lighter run: %d (%.1f%% of the passes)' % (len(light), 100.0 * len(light) / len(up)))
        if used:
            w('- engine GPU (ms): ' + dist([e[3] / 100.0 for e in used if e[3]]))
            w('- motion estimate (ms): ' + dist([e[4] / 100.0 for e in used if e[4]]))
        w('')

    # auto quality and hotkeys
    auto = [e for e in events if e[1] == 'auto']
    if auto:
        w('## Auto quality')
        w('')
        for e in auto[:40]:
            w('- %.3f s: %s' % (e[0] / 1e6, ('model resolution %d %% (model %.1f ms)' % (e[2], e[4] / 100.0)) if e[2] else 'the model now runs on every %d frame(s)' % e[3]))
        w('')
    keys = [e for e in events if e[1] == 'hotkey']
    if keys:
        names = {0: 'before/after (A/B)', 1: 'split view', 2: 'sharpness down', 3: 'sharpness up', 4: 'look preset', 5: 'screenshot / pair', 6: 'save the recording'}
        w('## Hotkeys')
        w('')
        for e in keys[:40]: w('- %.3f s: %s' % (e[0] / 1e6, names.get(e[2], 'key %d' % e[2])))
        w('')
    return '\n'.join(out)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--out')
    ap.add_argument('--gaps', type=int, default=15)
    ap.add_argument('--window', type=int, default=60, help='milliseconds before a gap ends to look for other events')
    a = ap.parse_args()
    events, meta = load(a.trace)
    text = report(events, meta, a.gaps, a.window)
    if a.out:
        with open(a.out, 'w', encoding='utf-8') as f: f.write(text + '\n')
        print('report written to', a.out)
    else:
        sys.stdout.buffer.write((text + '\n').encode('utf-8'))

if __name__ == '__main__':
    main()
