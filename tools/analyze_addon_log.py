#!/usr/bin/env python
"""Reads an addon's log (DLSS5NR01.log, DLSS4DLAA.log, FSR3UPSC.log, XESSUPSC.log) and prints what a report needs: how steady the game's frames were window by window, what the
model or the upscaler cost, what auto quality did, and anything that switched the addon off. Standard library only.

  python tools/analyze_addon_log.py <addon.log> [more logs ...] [--out report.md] [--bumpy 1.6]

Lines it reads (src/addon/runtime.cpp): "frame time over the last N frames: p50 ..." (Neural Rendering, every 300 frames), "game frame time over N real frames: average ..." (the upscalers,
every 600), "GPU on Lossless Scaling's queue ...", "auto: ...", and the lines that switch an addon off. A window is "bumpy" when its p95 is more than --bumpy times its p50 (1.6 by default):
the game's frames are uneven there, whatever the addon does.
"""
import argparse, re, statistics, sys

TIME = re.compile(r'^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]\s*(.*)$')
NR_WINDOW = re.compile(r'frame time over the last (\d+) frames: p50 ([\d.]+) ms, p95 ([\d.]+), p99 ([\d.]+), worst ([\d.]+) \| (\d+) frames over 20 ms \((\d+)%\), (\d+) over 33 ms \| model ([\d.]+) ms, GPU start \+([\d.]+) ms')
UP_WINDOW = re.compile(r'game frame time over (\d+) real frames: average ([\d.]+) ms \(([\d.]+) fps\), p50 ([\d.]+), p95 ([\d.]+), p99 ([\d.]+), worst ([\d.]+) \| (\w+) ([\d.]+) ms a presented frame \(motion ([\d.]+)\), (\d+) presented per real frame(?: \(each ([\d.]+) of a real step\))?')
COMPOSE = re.compile(r"GPU on Lossless Scaling's queue \(ms\): handing over ([\d.]+) .*\| compose ([\d.]+)")
AUTO = re.compile(r'auto: (.*)')
OFF = re.compile(r'(DISABLED|switched off|switching off|watchdog|exception|engine failed|stall|not responding|failed to (create|start))', re.I)

def secs(m):
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000.0

def clock(t):
    return '%02d:%02d:%02d' % (t // 3600, t // 60 % 60, int(t) % 60)

def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))] if v else float('nan')

def read(path):
    out = {'nr': [], 'up': [], 'compose': [], 'auto': [], 'off': [], 'starts': [], 'lines': 0}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = TIME.match(line.rstrip('\n'))
            if not m: continue
            out['lines'] += 1
            t, text = secs(m), m.group(5)
            if 'initialised' in text and 'host version' in text: out['starts'].append(t)
            if text.startswith('[ngx]'): continue
            w = NR_WINDOW.search(text)
            if w:
                n, p50, p95, p99, worst, o20, o20p, o33, model, start = w.groups()
                out['nr'].append((t, int(n), float(p50), float(p95), float(p99), float(worst), int(o20p), int(o33), float(model), float(start)))
                continue
            w = UP_WINDOW.search(text)
            if w:
                n, avg, fps, p50, p95, p99, worst, name, gpu, motion, per, step = w.groups()
                out['up'].append((t, int(n), float(avg), float(fps), float(p50), float(p95), float(p99), float(worst), name, float(gpu), float(motion), int(per), float(step) if step else 0.0))
                continue
            w = COMPOSE.search(text)
            if w: out['compose'].append((t, float(w.group(1)), float(w.group(2)))); continue
            w = AUTO.match(text)
            if w: out['auto'].append((t, w.group(1))); continue
            if OFF.search(text) and 'requirements:' not in text: out['off'].append((t, text[:200]))
    return out

def report(path, bumpy):
    d = read(path)
    L = []
    w = L.append
    w('## %s' % path)
    w('')
    w('%d lines, %d start(s)%s.' % (d['lines'], len(d['starts']), (' (' + ', '.join(clock(t) for t in d['starts']) + ')') if d['starts'] else ''))
    w('')
    if d['nr']:
        p50s = [x[2] for x in d['nr']]; p95s = [x[3] for x in d['nr']]
        w('### Game frame time, window by window (Neural Rendering, %d windows of about 300 frames)' % len(d['nr']))
        w('')
        w('- typical frame (median of the windows\' p50): %.1f ms (%.0f fps); worst window p50 %.1f ms' % (statistics.median(p50s), 1000 / statistics.median(p50s), max(p50s)))
        w('- p95 of the windows: median %.1f ms, worst %.1f ms; p99 worst %.1f ms; slowest frame %.1f ms' % (statistics.median(p95s), max(p95s), max(x[4] for x in d['nr']), max(x[5] for x in d['nr'])))
        bump = [x for x in d['nr'] if x[3] > bumpy * x[2] and x[3] - x[2] > 6]
        w('- bumpy windows (p95 over %.1fx p50): %d of %d' % (bumpy, len(bump), len(d['nr'])))
        w('- model: median %.1f ms, worst %.1f ms; its GPU start delay (how long it waited for the card): median %.1f ms, worst %.1f ms' % (
            statistics.median(x[8] for x in d['nr']), max(x[8] for x in d['nr']), statistics.median(x[9] for x in d['nr']), max(x[9] for x in d['nr'])))
        late = [x for x in d['nr'] if x[9] > 4.0]
        w('- windows where the model waited over 4 ms for the card: %d of %d%s' % (len(late), len(d['nr']), ' (the graphics card is full: the game and the model compete)' if len(late) > len(d['nr']) / 2 else ''))
        w('')
        w('| time | p50 ms | p95 | p99 | worst | over 33 ms | model ms | GPU start ms | |')
        w('|---|---|---|---|---|---|---|---|---|')
        for x in sorted(d['nr'], key=lambda x: -(x[3] / max(x[2], 0.1)))[:12]:
            w('| %s | %.1f | %.1f | %.1f | %.1f | %d | %.1f | %.1f | %s |' % (clock(x[0]), x[2], x[3], x[4], x[5], x[7], x[8], x[9], 'bumpy' if x in bump else ''))
        w('')
        w('(the 12 windows with the largest p95 / p50)')
        w('')
    if d['up']:
        w('### Game frame time and the upscaler\'s cost (%d windows of 600 real frames)' % len(d['up']))
        w('')
        w('- typical frame: p50 median %.1f ms (%.1f fps on average); p95 median %.1f ms, worst %.1f ms; slowest frame %.1f ms' % (
            statistics.median(x[4] for x in d['up']), statistics.fmean(x[3] for x in d['up']), statistics.median(x[5] for x in d['up']), max(x[5] for x in d['up']), max(x[7] for x in d['up'])))
        bump = [x for x in d['up'] if x[5] > bumpy * x[4] and x[5] - x[4] > 6]
        w('- bumpy windows (p95 over %.1fx p50): %d of %d' % (bumpy, len(bump), len(d['up'])))
        w('- the upscaler (%s): %.2f ms of GPU for a presented frame (median; worst %.2f), motion estimate %.2f ms; presented per real frame: %s' % (
            d['up'][0][8], statistics.median(x[9] for x in d['up']), max(x[9] for x in d['up']), statistics.median(x[10] for x in d['up']), ', '.join(sorted(set(str(x[11]) for x in d['up'])))))
        steps = [x[12] for x in d['up'] if x[12]]
        if steps: w('- each presented picture is %.2f of a real frame\'s step (median); with a whole multiplier this is 1 / the multiplier' % statistics.median(steps))
        w('')
    if d['compose']:
        w('### Lossless Scaling\'s queue (Neural Rendering)')
        w('')
        w('- compose pass per presented frame: median %.2f ms, worst %.2f ms; handing a frame over: median %.3f ms' % (
            statistics.median(x[2] for x in d['compose']), max(x[2] for x in d['compose']), statistics.median(x[1] for x in d['compose'])))
        w('')
    if d['auto']:
        w('### Auto quality (%d lines)' % len(d['auto']))
        w('')
        for t, text in d['auto'][:25]: w('- %s  %s' % (clock(t), text))
        if len(d['auto']) > 25: w('- ... %d more' % (len(d['auto']) - 25))
        w('')
    if d['off']:
        w('### What switched things off or failed (%d lines)' % len(d['off']))
        w('')
        for t, text in d['off'][:25]: w('- %s  %s' % (clock(t), text))
        w('')
    if not (d['nr'] or d['up']):
        w('No frame-time windows in this log (the game was not scaled long enough: a window is 300 frames for Neural Rendering and 600 for the upscalers).')
        w('')
    return '\n'.join(L)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('logs', nargs='+')
    ap.add_argument('--out')
    ap.add_argument('--bumpy', type=float, default=1.6)
    a = ap.parse_args()
    text = '# Addon log report\n\n' + '\n'.join(report(p, a.bumpy) for p in a.logs)
    if a.out:
        with open(a.out, 'w', encoding='utf-8') as f: f.write(text + '\n')
        print('report written to', a.out)
    else:
        sys.stdout.buffer.write((text + '\n').encode('utf-8'))

if __name__ == '__main__':
    main()
