"""Runs the offline test host (nr_hosttest.exe) through a set of scenarios and checks the presented frame it writes.

No window is shown and nothing touches Lossless Scaling or a game: the host loads the built addon DLL, feeds it a synthetic
LSFG pattern and a hidden swap chain, and dumps one presented (generated) frame to present_gen.bmp. Every scenario compares
that frame with the known synthetic pattern (or with the baseline scenario).

It uses the GPU for about half a minute per scenario, so do not run it while a game is running.

    python run_hosttest_matrix.py [--nr <addon build folder>] [--snippet <path to nvngx_dlssnr.dll>]
                                  [--out OUTDIR] [--only name,name] [--list]
"""
import argparse
import re
import os
import shutil
import subprocess
import sys
import time

import numpy as np

W, H = 1920, 1080
DEFAULT_NR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'addons', 'DLSS5NR01', 'build', 'Release')
# Lossless Scaling's folder: LS_DIR, else the first of the usual places that holds the model (Steam's, or where it was moved to)
_LS_DIRS = [os.environ.get('LS_DIR', ''), r'C:\Program Files (x86)\Steam\steamapps\common\Lossless Scaling', r'D:\Utilities\Lossless Scaling']
DEFAULT_SNIPPET = next((os.path.join(d, 'nvngx_dlssnr.dll') for d in _LS_DIRS if d and os.path.isfile(os.path.join(d, 'nvngx_dlssnr.dll'))),
                       os.path.join(_LS_DIRS[1], 'nvngx_dlssnr.dll'))


def pattern():
    """The synthetic frame the host feeds LSFG (see Pattern() in addon_host_test.cpp), as float RGB 0..255, shape (H, W, 3)."""
    x = np.arange(W)[None, :].repeat(H, 0)
    y = np.arange(H)[:, None].repeat(W, 1)
    r = (x * 255 // W).astype(np.float32)
    g = (y * 255 // H).astype(np.float32)
    b = np.where(((x // 32 + y // 32) & 1) == 1, 200, 60).astype(np.float32)
    return np.stack([r, g, b], -1)


def load_bmp(path):
    """32-bit top-down BMP written by the host (BGRA) -> float RGB (H, W, 3)."""
    with open(path, 'rb') as f:
        data = f.read()
    off = int.from_bytes(data[10:14], 'little')
    w = int.from_bytes(data[18:22], 'little', signed=True)
    h = abs(int.from_bytes(data[22:26], 'little', signed=True))
    px = np.frombuffer(data, np.uint8, count=w * h * 4, offset=off).reshape(h, w, 4)
    return px[..., [2, 1, 0]].astype(np.float32)


def luma(a):
    return a @ np.array([0.299, 0.587, 0.114], np.float32)


class Result:
    def __init__(self):
        self.lines = []
        self.ok = True

    def check(self, name, cond, detail=''):
        self.lines.append(('PASS' if cond else 'FAIL') + '  ' + name + (('  (' + detail + ')') if detail else ''))
        self.ok &= bool(cond)


def run_host(nr_dir, snippet, keys, out_dir, tag):
    exe = os.path.join(nr_dir, 'nr_hosttest.exe')
    # addon=<dll> picks the addon of the pair to load (DLSS 5 Neural Rendering by default); every other key goes to the host.
    # The DLLs are given by full path and the host runs in a folder of the scenario's own (it writes its frames there), so scenarios can run side by side.
    dll = os.path.join(nr_dir, next((k[6:] for k in keys if k.startswith('addon=')), 'DLSS5NR01.dll'))
    keys = ['second=' + os.path.join(nr_dir, k[7:]) if k.startswith('second=') else k for k in keys if not k.startswith('addon=')]
    args = [exe, dll, '-', snippet] + keys
    work = os.path.join(out_dir, 'run_' + tag)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    t0 = time.time()
    try:
        proc = subprocess.run(args, cwd=work, capture_output=True, text=True, errors='replace', timeout=240)
        rc, text = proc.returncode, proc.stdout + proc.stderr
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode('utf-8', 'replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
        rc, text = -1, out + '\nTIMED OUT after 240 s'
    with open(os.path.join(out_dir, tag + '.log'), 'w', encoding='utf-8') as f:
        f.write(text)
    bmp = os.path.join(work, 'present_gen.bmp')
    frame = None
    if os.path.exists(bmp):
        shutil.copyfile(bmp, os.path.join(out_dir, tag + '.bmp'))
        frame = load_bmp(bmp)
    return rc, text, frame, time.time() - t0


def basic(res, rc, text):
    res.check('exit code 0', rc == 0, 'rc=%s' % rc)
    res.check('tap is read-only', 'TAP READ-ONLY' in text)
    res.check('no crash / engine failure', 'NrEngine FAILED' not in text and 'CRASH' not in text and 'DISABLED' not in text)


def motion_counts(text):
    """The last 'motion vectors so far' line: (this frame's flow, the previous frame's, dropped waiting), or None."""
    import re
    m = re.findall(r"motion vectors so far: this frame's flow (\d+), the previous frame's (\d+), frames dropped waiting for a flow pass (\d+)", text)
    return tuple(int(v) for v in m[-1]) if m else None


def scenario_present_mode(ctx, res, text, frame):
    # frame generation switched off for 6 s: after a few presents without a capture the model takes the presented frames and its result goes
    # onto them again (the host's check-off line then counts changed pixels: the model's result, on the frame it was made from)
    res.check('with frame generation off the model takes the presented frames', 'frame generation is off: the model takes the presented frames' in text)
    off = re.search(r'\[check-off\] .*?: (\d+) pixels changed', text)
    res.check('...and its result goes onto them', off is not None and int(off.group(1)) > 0, off.group(0)[12:] if off else 'no check-off line')
    res.check('...with no failure', 'SwitchOff' not in text and 'switched off:' not in text.lower() and 'bridge init failed' not in text)


def scenario_hdr(ctx, res, text, frame):
    # frame generation off and the presented frames HDR (scRGB or HDR10): the model takes their SDR view and only its change goes back
    line = re.search(r'\[check-hdr\] .*', text)
    res.check('the HDR frames reach the model', 'unsupported frame format' not in text and "cannot be given to the model" not in text and line is not None,
              line.group(0)[12:] if line else 'no check-hdr line')
    res.check('...its result goes onto them', 'HDR COMPOSED' in text)
    res.check('...highlights far above SDR white stay as they were', 'HIGHLIGHTS KEPT' in text)
    res.check('...and no NaN or infinity is written', '(CLEAN)' in text)
    res.check('...with no failure', 'SwitchOff' not in text and 'switched off:' not in text.lower() and 'Compose11: HLSL' not in text)


def scenario_record_replay(ctx, res, text, frame):
    # the recorder on during the run: it saves by itself once it holds 60 frames; the file is then looked into (nr_lsrec info) and played back
    # through the addon in a second run of the host
    import glob
    saved = re.search(r'recorder: Saved ([0-9.]+) s \((\d+) frames, .*? to (.+\.lsrec)', text)
    res.check('the recorder saved the last frames', saved is not None, saved.group(0)[10:] if saved else 'no "recorder: Saved" line')
    rec_lines = [l for l in text.splitlines() if 'recorder:' in l]
    res.check('...with no failure', not any('could not' in l or 'cannot' in l or 'did not finish' in l for l in rec_lines))
    if not saved:
        return
    path = saved.group(3).strip()
    tool = os.path.join(ctx['nr'], 'nr_lsrec.exe')
    info = subprocess.run([tool, 'info', path], capture_output=True, text=True, timeout=60)
    frames = re.search(r'(\d+) frames over ([0-9.]+) s', info.stdout)
    res.check('nr_lsrec reads it: the frames and their time', info.returncode == 0 and frames is not None and int(frames.group(1)) >= 60,
              (frames.group(0) if frames else info.stdout.strip()[:200]))
    out = os.path.join(ctx['out'], 'replay').replace('\\', '/')
    rc, rtext, _, secs = run_host(ctx['nr'], ctx['snippet'], ['offframes=150', 'replay=' + path.replace('\\', '/'), 'replayOut=' + out], ctx['out'], 'record_replay_second')
    line = re.search(r'\[check-replay\] .*', rtext)
    res.check('played back through the addon, its result on the frames', rc == 0 and 'REPLAY COMPOSED' in rtext, (line.group(0)[15:] if line else 'no check-replay line') + ' (%.0f s)' % secs)
    res.check('...and pictures of it written', len(glob.glob(os.path.join(out, 'replay_*.bmp'))) > 0)


def scenario_tone(ctx, res, text, frame):
    # the upscalers' brightness (+0.15) on the frame before it is upscaled: the picture replaces NIS's and is brighter; with Neural Rendering
    # on (the manager's switch for it, _enabled=1 here) the upscaler leaves the tone to it and the picture is as without
    m = re.search(r'\[check-nis\] .*?: ([0-9.]+)% of the output is the fake NIS pass.s magenta, average colour off by ([0-9.]+) levels', text)
    res.check('the upscaled picture replaces NIS\'s', m is not None and float(m.group(1)) < 1.0, m.group(0)[12:] if m else 'no check-nis line')
    if not m:
        return
    off = float(m.group(2))
    if '_enabled=1' in ctx['keys']:
        res.check('...with Neural Rendering on, the tone is left to it (the colour as without)', off < 3.0, '%.2f levels off' % off)
    else:
        res.check('...brighter by about 0.15 (38 levels)', 25.0 < off < 45.0, '%.2f levels off' % off)


def scenario_colour(ctx, res, text, frame):
    # the upscalers' saturation at 0 on the frame before it is upscaled: the picture is grey (its neighbour-to-neighbour detail, colour
    # included, falls from about 1.3 to about 0.4) and it still replaces NIS's
    m = re.search(r'\[check-nis\] .*?: ([0-9.]+)% of the output .*? detail ([0-9.]+)', text)
    res.check("the upscaled picture replaces NIS's", m is not None and float(m.group(1)) < 1.0, m.group(0)[12:] if m else 'no check-nis line')
    res.check('...and it is grey (saturation 0)', m is not None and float(m.group(2)) < 0.7, 'detail %s' % (m.group(2) if m else '?'))


def scenario_runtime_switch(ctx, res, text, frame):
    # the runtime file chosen in the manager's Runtimes list changes while the upscaler runs: it must start again on the new file and go on
    # replacing NIS (an FSR runtime tells which version it runs)
    res.check('the new choice is seen', 'is chosen: the engine starts again on it' in text)
    res.check('...the engine starts again', text.count('engine started') >= 2, '%d starts' % text.count('engine started'))
    if 'fsrRuntime' in ' '.join(ctx['keys']):
        m = re.findall(r'the runtime runs (\S+)', text)
        res.check('...on FSR 4.1.1b (OptiScaler), after 3.1.4', m[:1] == ['3.1.4'] and m[-1:] == ['4.1.1b'], ', '.join(m))
    res.check('...and still replaces NIS', 'REPLACED NIS' in text)


def scenario_runtime_crashed(ctx, res, text, frame):
    # the marker said the chosen runtime was being tried when Lossless Scaling went down: it is not used again until another is chosen; the shipped one upscales
    res.check('the chosen runtime that did not finish its last start is not used', 'did not finish its last start' in text)
    res.check('...the shipped one upscales instead', 'REPLACED NIS' in text)


def scenario_bars(ctx, res, text, frame):
    # Lossless Scaling draws a 1920x1080 frame into a 2560x1080 screen with black bars: the compose works out where, checks the bars are dark, and applies its change in the picture only
    res.check('the compose works out where the frame is drawn', bool(re.search(r'compose: the 1920x1080 frame is drawn into 2560x1080 with bars', text)))
    res.check('...and the strips through the bars are seen to be dark', 'compose: the bars are there' in text)
    comp = re.findall(r'composed (\d+)', text)
    res.check("...and the model's change is composed", bool(comp) and max(int(c) for c in comp) > 20, ', '.join(comp[-3:]))


def scenario_every2(ctx, res, text, frame):
    # the setting "Run the model" at every 2nd frame: the addon says so, and the model runs on about half the real frames (the frames between are shown with its last result)
    res.check('the addon puts the model on every 2nd frame', 'the model now runs on every 2nd frame (the setting Run the model)' in text)
    m = re.findall(r'tap #(\d+): .*? runs (\d+) skipped', text)
    if m:
        taps, runs = int(m[-1][0]), int(m[-1][1])
        res.check('the model runs on about half of the real frames', taps >= 40 and 0.35 <= runs / taps <= 0.65, '%d runs in %d taps' % (runs, taps))
    else:
        res.check('the model runs on about half of the real frames', False, 'no tap line in the log')
    res.check('compose applied', 'COMPOSE APPLIED' in text)


def scenario_base(ctx, res, text, frame):
    res.check('compose applied', 'COMPOSE APPLIED' in text)
    mc = motion_counts(text)
    res.check("the model gets this frame's own motion (fresh flow, the default)", bool(mc) and mc[0] > 0 and mc[0] >= 9 * (mc[0] + mc[1]) // 10 and mc[2] <= 2,
              'fresh %d, previous %d, dropped %d' % mc if mc else 'no motion line in the log')
    res.check('tap followed the resolution change', 'TAP FOLLOWED' in text)
    held = re.search(r'held up a dispatch while the model was made again: ([0-9.]+ ms)', text)
    res.check('making the model again for the new size did not hold up the render thread', 'NO STALL' in text, held.group(1) if held else 'no line in the log')
    res.check('with frame generation off, the last result does not stay on the screen', 'OLD RESULT DROPPED' in text)
    res.check('the addon reports live metrics and a status to the host', 'LIVE METRICS OK' in text)
    d = np.abs(frame - ctx['pat']).mean()
    ctx['base'] = frame
    res.check('model changed the picture', d > 0.3, 'mean abs change %.2f' % d)


def scenario_hud(ctx, res, text, frame):
    left = np.abs(frame[:, : W // 2 - 8] - ctx['pat'][:, : W // 2 - 8]).max()
    right = np.abs(frame[:, W // 2 + 8:] - ctx['pat'][:, W // 2 + 8:]).mean()
    res.check('protected left half is bit-identical to the original', left == 0, 'max diff %.1f' % left)
    res.check('unprotected right half is still enhanced', right > 0.3, 'mean abs change %.2f' % right)


def scenario_sharpen(ctx, res, text, frame):
    def hf(a):  # high-frequency energy of the luma
        l = luma(a)
        return np.abs(l[:, 1:] - l[:, :-1]).mean()
    res.check('sharpen adds high-frequency energy', hf(frame) > hf(ctx['base']) * 1.02, '%.3f vs %.3f' % (hf(frame), hf(ctx['base'])))


def scenario_shadows_up(ctx, res, text, frame):
    lp, lb, lf = luma(ctx['pat']), luma(ctx['base']), luma(frame)
    dark, bright = lp < 60, lp > 190
    gd, gb = (lf[dark] - lb[dark]).mean(), (lf[bright] - lb[bright]).mean()
    res.check('shadows +1 lifts the dark areas', gd > 8, 'mean luma change %.1f' % gd)
    res.check('shadows +1 leaves the bright areas alone', abs(gb) < 3, 'mean luma change %.1f' % gb)


def scenario_highlights_down(ctx, res, text, frame):
    lp, lb, lf = luma(ctx['pat']), luma(ctx['base']), luma(frame)
    dark, bright = lp < 60, lp > 190
    gb, gd = (lf[bright] - lb[bright]).mean(), (lf[dark] - lb[dark]).mean()
    res.check('highlights -1 pulls the bright areas down', gb < -8, 'mean luma change %.1f' % gb)
    res.check('highlights -1 leaves the dark areas alone', abs(gd) < 3, 'mean luma change %.1f' % gd)


def scenario_grain(ctx, res, text, frame):
    diff = (luma(frame) - luma(ctx['base']))
    res.check('grain adds noise', diff.std() > 3, 'std of change %.2f' % diff.std())
    res.check('grain averages out (mean change small)', abs(diff.mean()) < 3, 'mean %.2f' % diff.mean())


def scenario_grain_hud(ctx, res, text, frame):
    left = np.abs(frame[:, : W // 2 - 8] - ctx['pat'][:, : W // 2 - 8]).max()
    res.check('grain does not touch a protected area', left == 0, 'max diff %.1f' % left)


def scenario_smooth(ctx, res, text, frame):
    d = np.abs(frame - ctx['pat']).mean()
    res.check('smoothed delta still changes the picture', d > 0.2, 'mean abs change %.2f' % d)
    res.check('smoothed delta is not wildly different from unsmoothed', abs(d - np.abs(ctx['base'] - ctx['pat']).mean()) < 3.0)


def scenario_smooth_passes(ctx, res, text, frame):
    # three model passes change the picture more than one (measured earlier: 6.6 / 12.5 / 17.6 for 1 / 2 / 3 passes), so the
    # comparison is "clearly stronger than one pass, and not absurd"
    d = np.abs(frame - ctx['pat']).mean()
    b = np.abs(ctx['base'] - ctx['pat']).mean()
    res.check('3 passes with smoothing change the picture more than 1 pass', d > b * 1.5, '%.2f vs %.2f' % (d, b))
    res.check('3 passes with smoothing stay in a sane range', d < 40, 'mean abs change %.2f' % d)


def _halves(frame, pat):
    """Mean absolute change against the pattern in the left and right half (clear of the middle and the edges)."""
    l = np.abs(frame[:, 16: W // 2 - 16] - pat[:, 16: W // 2 - 16]).mean()
    r = np.abs(frame[:, W // 2 + 16: W - 16] - pat[:, W // 2 + 16: W - 16]).mean()
    return l, r


def scenario_ghost_off(ctx, res, text, frame):
    l, r = _halves(frame, ctx['pat'])
    ctx['ghost_off'] = (l, r)
    res.check('guard off: both halves are enhanced', l > 0.3 and r > 0.3, 'left %.2f right %.2f' % (l, r))


def scenario_ghost_on(ctx, res, text, frame):
    l, r = _halves(frame, ctx['pat'])
    lo, ro = ctx['ghost_off']
    res.check('guard on: the half whose motion fields agree keeps its enhancement', l > 0.75 * lo, '%.2f vs %.2f without the guard' % (l, lo))
    res.check('guard on: the half whose motion fields disagree is faded out', r < 0.25 * ro, '%.2f vs %.2f without the guard' % (r, ro))


def scenario_none(ctx, res, text, frame):
    pass


def scenario_flow_previous(ctx, res, text, frame):
    mc = motion_counts(text)
    res.check("with fresh flow off the model gets the previous frame's motion, as before", bool(mc) and mc[0] == 0 and mc[1] > 0,
              'fresh %d, previous %d, dropped %d' % mc if mc else 'no motion line in the log')
    res.check('...and it still changes the picture', np.abs(frame - ctx['pat']).mean() > 0.3)


def scenario_scaler(ctx, res, text, frame):
    # the DLSS Upscaler in place of Lossless Scaling's NIS pass (a fake one here, which paints its output magenta)
    made = re.search(r'DLSS upscaler: (\d+)x(\d+) -> (\d+)x(\d+).*?made in (\d+) ms', text)
    res.check('DLSS runs on a D3D12 device of its own, never on Lossless Scaling\'s', 'ready on its own D3D12 device' in text)
    res.check('it finds the NIS pass and makes DLSS for its sizes', bool(made) and made.group(1, 2, 3, 4) == ('1920', '1080', '2880', '1620'),
              made.group(0) if made else 'no line in the log')
    nis = re.search(r'\[check-nis\].*', text)
    res.check('DLSS writes a real upscaled picture where the NIS pass would have', 'DLSS REPLACED NIS' in text, nis.group(0)[12:] if nis else 'no check line')
    res.check('...on every presented frame, real and generated', bool(re.search(r'DLSS scaler: \d+ frames upscaled, NIS passes seen \d+, 2 per real frame', text)))
    detail = re.search(r'detail ([0-9.]+)', text)
    if detail:
        if 'sharpen=0.5' in ' '.join(ctx.get('keys', [])):
            res.check('sharpening after DLSS raises the fine detail', 'scaler_detail' in ctx and float(detail.group(1)) > ctx['scaler_detail'] * 1.05,
                      '%s against %.3f without' % (detail.group(1), ctx.get('scaler_detail', 0)))
        elif 'dlaaPreset=13' not in ' '.join(ctx.get('keys', [])):
            ctx['scaler_detail'] = float(detail.group(1))


def scenario_scaler_light(ctx, res, text, frame):
    # the generated frames (a generated-frame pass before the first NIS pass of each real frame) get the lighter run; the picture that replaces NIS is still a real, upscaled one
    res.check('a generated frame is recognised and given the lighter run', 'lighter upscaling: the first generated frame was given the lighter run' in text)
    nis = re.search(r'\[check-nis\].*', text)
    res.check("the pictures that replace NIS are still DLSS's, real and generated", 'DLSS REPLACED NIS' in text, nis.group(0)[12:] if nis else 'no check line')
    res.check('...and nothing failed', 'could not' not in text.lower() or 'could not hook' in text.lower())


def scenario_scaler_noflow(ctx, res, text, frame):
    # frame generation off: no capture or flow passes, NIS on the BGRA8 frame
    nis = re.search(r'\[check-nis\].*', text)
    res.check('with frame generation off (no flow, a BGRA8 frame) DLSS still replaces NIS', 'DLSS REPLACED NIS' in text, nis.group(0)[12:] if nis else 'no check line')
    still = re.search(r'motion estimator: over \d+ frames, average vector \((-?[0-9.]+), (-?[0-9.]+)\) px, average length ([0-9.]+) px', text)
    if still and 'nismove=1' not in ctx.get('keys', []):   # a still picture: the estimate must be exactly still (a small wrong offset blurs text)
        res.check('on a still picture the measured motion is zero', float(still.group(3)) < 0.01, 'average length %s px' % still.group(3))


def move_error(text):
    m = re.search(r'\[check-move\] the picture is off the moving picture by ([0-9.]+) levels', text)
    return float(m.group(1)) if m else None


def scenario_move_none(ctx, res, text, frame):
    scenario_scaler_noflow(ctx, res, text, frame)
    err = move_error(text)
    res.check('a sliding picture is measured against the moving picture', err is not None, '%s levels' % err)
    if err is not None:
        ctx['move_error_none'] = err


def scenario_move(ctx, res, text, frame):
    # the picture slides 5.37 px right and 2.21 px down a frame: each pixel was that far left and up in the frame before
    scenario_scaler_noflow(ctx, res, text, frame)
    est = re.search(r'motion estimator: over \d+ frames, average vector \((-?[0-9.]+), (-?[0-9.]+)\) px, average length ([0-9.]+) px, match cost ([0-9.]+); motion ([0-9.]+) ms', text)
    res.check('the motion estimator runs', est is not None, est.group(0)[18:] if est else 'no estimator line')
    if est:
        x, y = float(est.group(1)), float(est.group(2))
        res.check('...and finds the slide: (-5.37, -2.21) px a frame', abs(x + 5.37) < 0.35 and abs(y + 2.21) < 0.35, '(%.2f, %.2f)' % (x, y))
    err = move_error(text)
    none = ctx.get('move_error_none')
    # Until 0.9.13 the measured motion beat "none" by 15 % here. It cannot any more, by design: Lossless Scaling's frames carry no sub-pixel camera
    # jitter, so DLSS's history adds no detail however well it follows the picture (nr_sreval on recorded games: DLSS scores no better, even worse,
    # with our vectors than with none), and the upscaler leans on the new frame wherever the picture moves (kFastMotionShare*). What this checks now
    # is that the motion path does no harm: the sliding picture is no further off with the measured motion (and the lean) than with none, within the
    # run-to-run spread of this synthetic measure (about a level and a half).
    res.check('with the measured motion, DLSS is no further off the sliding picture than with none (+20 %)', err is not None and none is not None and err < none * 1.2,
              '%s against %s levels without' % (err, none))


def scenario_viewport(ctx, res, text, frame, name='DLSS'):
    # a 4:3 window on a 16:9 screen: NIS scales into the middle; the upscaler must read NIS's viewports, fill the middle and leave the borders
    if name == 'DLSS': scenario_scaler_noflow(ctx, res, text, frame)
    elif name.startswith('FSR'): scenario_fsr(ctx, res, text, frame)   # (XeSS's own checks come from scenario_xess_viewport)
    res.check("the upscaler reads NIS's viewports from its constants", 'the upscaler takes that part' in text)
    vp = re.search(r'\[check-vp\] .*?: (\d+) pixels of the borders lit', text)
    res.check("...and leaves Lossless Scaling's borders as they are", vp is not None and int(vp.group(1)) == 0, vp.group(0)[11:] if vp else 'no check-vp line')


def scenario_viewport_decoy(ctx, res, text, frame):
    # issue 13 again (3440x1440): a second pass a frame that looks like NIS (48 bytes of constants, one group fewer across) is refused; it must not disturb the real one, whose
    # constants are read once, not every frame (before 0.9.31: one reader for both, 260 lines in this run and the real pass never taken)
    scenario_viewport(ctx, res, text, frame)
    lines = text.count("NIS pass on part of its output")
    res.check("the NIS-looking passes are read once each, not every frame", lines <= 8, "%d lines" % lines)


def scenario_fsr_viewport(ctx, res, text, frame):
    scenario_viewport(ctx, res, text, frame, 'FSR 3')


def scenario_xess_viewport(ctx, res, text, frame):
    scenario_xess(ctx, res, text, frame, whole=False)
    scenario_viewport(ctx, res, text, frame, 'XeSS')


def edge_error(text):
    m = re.search(r'\[check-edge\] the slanted edge is off the ideal smooth edge by ([0-9.]+) levels over (\d+) pixels', text)
    return (float(m.group(1)), int(m.group(2))) if m else None


def scenario_fsr_aliased(ctx, res, text, frame):
    scenario_fsr(ctx, res, text, frame)
    e = edge_error(text)
    res.check('a hard slanted edge is measured against the ideal smooth one', e is not None and e[1] > 1000, '%.2f levels over %d pixels' % e if e else 'no check-edge line')
    if e:
        ctx['edge_error_none'] = e[0]


def scenario_fsr_edges(ctx, res, text, frame):
    scenario_fsr(ctx, res, text, frame)
    res.check('edge smoothing runs and is timed', re.search(r'after the upscaler [0-9.]+ ms \(edge smoothing 1\.00', text) is not None)
    e = edge_error(text); none = ctx.get('edge_error_none')
    res.check('with edge smoothing the edge is closer to the ideal smooth edge', e is not None and none is not None and e[0] < none * 0.9,
              '%.2f against %s levels without' % (e[0], none) if e else 'no check-edge line')


def scenario_fsr_edges_sharp(ctx, res, text, frame):
    scenario_fsr(ctx, res, text, frame)
    res.check('edge smoothing and the sharpening pass run one after the other', re.search(r'after the upscaler [0-9.]+ ms \(edge smoothing 1\.00, sharpening 1.60', text) is not None)


def scenario_scaler_edges(ctx, res, text, frame):
    scenario_scaler_noflow(ctx, res, text, frame)
    res.check('edge smoothing runs with DLSS too', re.search(r'after the upscaler [0-9.]+ ms \(edge smoothing 1\.00', text) is not None)


def line_error(text):
    m = re.search(r'\[check-line\] near the swaying line the picture is off the true one by ([0-9.]+) levels', text)
    return float(m.group(1)) if m else None


def scenario_fsr_line(ctx, res, text, frame):
    res.check('the FSR Upscaler runs', 'FSR upscaler ready on its own D3D12 device' in text and 'FSR dispatch failed' not in text)
    res.check('...in place of the NIS pass', 'DLSS REPLACED NIS' in text)
    e = line_error(text)
    res.check('a thin swaying line is measured against the true picture', e is not None, '%s levels' % e)
    if e is None:
        return
    if 'FSR upscaler: stability 0.00' in text:
        ctx['line_error_plain'] = e
    elif ctx.get('line_error_plain') is not None:
        plain = ctx['line_error_plain']
        res.check('...and stability does not smear it (the thin moving line keeps leaning on the current frame)', e < plain * 1.05, '%.2f against %.2f without' % (e, plain))


def scenario_scaler_not_nvidia(ctx, res, text, frame):
    # Lossless Scaling on a card that is not NVIDIA's (WARP here): DLSS cannot start, NIS stays, and the addon says why and what to do
    # (0.9.6 only said "not running yet", which looked like it ran and did nothing)
    res.check("DLSS does not start on a card that is not NVIDIA's", 'DLSS upscaler: engine started' not in text and 'DLSS REPLACED NIS' not in text)
    res.check('...and says why, naming the card and the way out', bool(re.search(r"DLSS upscaler: Lossless Scaling runs on .+, not an NVIDIA card.*Preferred GPU.*FSR Upscaler", text)))


def scenario_scaler_hdr(ctx, res, text, frame):
    # the upscaler on HDR frames (scRGB or HDR10): it replaces NIS there too, keeps the 1000-nit highlights and the picture's brightness, and
    # writes no NaN (0.9.7 took 8-bit frames only and left NIS in place)
    m = re.search(r'\[check-nishdr\] (\w+) .*?: ([0-9.]+)% magenta \(([A-Z0-9 ]+)\), (\d+) NaN, the highlights ([0-9.]+) against ([0-9.]+) \(([A-Z ]+)\), the rest\'s mean ([0-9.]+) against ([0-9.]+) \(([A-Z ]+)\)', text)
    res.check('the HDR check ran', m is not None, 'no [check-nishdr] line')
    if not m:
        return
    res.check('the upscaler replaces NIS on %s frames' % m.group(1), m.group(3) == 'HDR REPLACED NIS', '%s%% magenta' % m.group(2))
    res.check('...writes no NaN', m.group(4) == '0', m.group(4))
    res.check('...keeps the highlights far above SDR white', m.group(7) == 'HIGHLIGHTS KEPT', '%s against %s' % (m.group(5), m.group(6)))
    res.check("...and the picture's brightness", m.group(10) == 'LEVEL KEPT', '%s against %s' % (m.group(8), m.group(9)))
    res.check('the frames are taken as HDR', 'HDR frames (' in text, 'no "HDR frames (" line')


def stable_checks(ctx, res, text, none_key, name):
    # Stability at 1: the upscaler must still follow a sliding picture (the slide is real motion, not flicker), and FSR takes its settings
    err = move_error(text)
    none = ctx.get(none_key)
    # FSR still beats "no motion" on the slide. DLSS cannot any more, by design (no camera jitter from Lossless Scaling: nothing for its history to add, and the
    # upscaler leans on the new frame; see scenario_move): there the check is that stability 1 does no harm, within this synthetic measure's run-to-run spread
    limit = 1.2 if name == 'DLSS' else 0.95
    res.check('with stability at 1, %s %s the sliding picture %s than with no motion' % (name, 'is no further off' if name == 'DLSS' else 'still follows', '(+20 %)' if name == 'DLSS' else 'better'),
              err is not None and none is not None and err < none * limit, '%s against %s levels without' % (err, none))


def scenario_stable(ctx, res, text, frame):
    scenario_scaler_noflow(ctx, res, text, frame)
    stable_checks(ctx, res, text, 'move_error_none', 'DLSS')


def scenario_fsr_stable(ctx, res, text, frame):
    scenario_fsr(ctx, res, text, frame)
    res.check('FSR 3 takes the stability settings', 'stability 1.00: velocity factor 0.50' in text and 'FSR refused' not in text)
    stable_checks(ctx, res, text, 'fsr_move_error_none', 'FSR 3')


def scenario_move_4k(ctx, res, text, frame):
    scenario_scaler_noflow(ctx, res, text, frame)
    est = re.search(r'motion estimator: over \d+ frames, average vector \((-?[0-9.]+), (-?[0-9.]+)\) px.*?; motion ([0-9.]+) ms of ([0-9.]+) ms', text)
    res.check('at 4K (DLAA) the estimate finds the slide: (-5.37, -2.21) px a frame', est is not None and abs(float(est.group(1)) + 5.37) < 0.35 and abs(float(est.group(2)) + 2.21) < 0.35,
              '(%s, %s), motion %s ms of %s ms' % est.groups() if est else 'no estimator line')
    stages = re.search(r'motion estimator: stages .*', text)
    res.check('...and times its stages', stages is not None, stages.group(0)[26:] if stages else 'no stages line')
    err = move_error(text)
    res.check('...and DLSS stays close to the moving picture', err is not None and err < 30, '%s levels' % err)


def scenario_fsr(ctx, res, text, frame):
    res.check('the FSR Upscaler loads AMD\'s runtime on a D3D12 device of its own', 'FSR upscaler ready on its own D3D12 device' in text)
    made = re.search(r'FSR upscaler: \d+x\d+ -> \d+x\d+ \(x[0-9.]+\), made in \d+ ms', text)
    res.check('...makes FSR 3 for the NIS pass\'s sizes', made is not None, made.group(0) if made else 'no line')
    res.check('...with no FSR 3 errors', 'FSR 3 error' not in text and 'FSR dispatch failed' not in text)
    scenario_scaler_noflow(ctx, res, text, frame)


def scenario_xess(ctx, res, text, frame, whole=True):
    res.check('the XeSS Upscaler loads Intel\'s runtime on a D3D12 device of its own', 'XeSS upscaler ready on its own D3D12 device' in text)
    made = re.search(r'XeSS upscaler: \d+x\d+ -> \d+x\d+ \(x[0-9.]+\), [a-z ]+, made in \d+ ms', text)
    res.check('...sets XeSS up for the NIS pass\'s sizes, at a quality setting that takes them', made is not None, made.group(0) if made else 'no line')
    res.check('...with no XeSS errors', 'XeSS execute failed' not in text and 'XeSS could not' not in text and 'XeSS takes no input' not in text)
    if whole:
        scenario_scaler_noflow(ctx, res, text, frame)


def scenario_xess_move_none(ctx, res, text, frame):
    # (only the baseline for xess_move: told that nothing moves while the picture slides, XeSS's picture drifts about 15 levels in average
    # colour, where DLSS's and FSR's stay within one, so it is not held to the replaced-NIS check here)
    scenario_xess(ctx, res, text, frame, whole=False)
    err = move_error(text)
    res.check('a sliding picture is measured against the moving picture', err is not None, '%s levels' % err)
    if err is not None:
        ctx['xess_move_error_none'] = err


def scenario_xess_move(ctx, res, text, frame):
    scenario_xess(ctx, res, text, frame)
    err = move_error(text)
    none = ctx.get('xess_move_error_none')
    res.check('with the measured motion, XeSS follows the sliding picture better than with none', err is not None and none is not None and err < none * 0.85,
              '%s against %s levels without' % (err, none))


def scenario_fsr_move_none(ctx, res, text, frame):
    scenario_fsr(ctx, res, text, frame)
    err = move_error(text)
    res.check('a sliding picture is measured against the moving picture', err is not None, '%s levels' % err)
    if err is not None:
        ctx['fsr_move_error_none'] = err


def scenario_fsr_move(ctx, res, text, frame):
    scenario_fsr(ctx, res, text, frame)
    err = move_error(text)
    none = ctx.get('fsr_move_error_none')
    res.check('with the measured motion, FSR 3 follows the sliding picture better than with none', err is not None and none is not None and err < none * 0.85,
              '%s against %s levels without' % (err, none))


def scenario_unload(ctx, res, text, frame):
    # the manager switches the upscaler off while it runs (AddonShutdown, FreeLibrary), then Lossless Scaling makes a new device: that hung once
    stop = re.search(r'upscaler stopped: the link in (\d+) ms, the engine in (\d+) ms', text)
    res.check('the upscaler stops promptly when switched off', stop is not None and int(stop.group(1)) + int(stop.group(2)) < 2000, stop.group(0) if stop else 'no stop line')
    un = re.search(r'\[check-unload\].*', text)
    res.check('...its DLL stays loaded and a new D3D11 device is made after it', un is not None and 'still loaded' in un.group(0) and 'was made' in un.group(0), un.group(0) if un else 'no unload line')


def scenario_pair(ctx, res, text, frame):
    # both addons loaded and switched on: the upscaler works beside Neural Rendering, which keeps its frames
    res.check('neither steps aside', 'BOTH ON' in text)
    res.check('Neural Rendering works on the frames as usual', 'COMPOSE APPLIED' in text and frame is not None and np.abs(frame - ctx['pat']).mean() > 0.3)


def scenario_selftest(ctx, res, text, frame):
    # the addon's own 'Test compatibility' path (started at start-up by the selfTestOnStart switch): nr_selftest.exe runs the model in its own process
    res.check('the addon ran the compatibility test and it passed with this model', 'compatibility test on' in text and ': passed (PASS)' in text)


# name, config overrides, checker
# Video Super Resolution (the prototype addon VSRUPSC, tools of the user's own NVIDIA Video Effects SDK): only run when the addon is built and the SDK is there (VFX_DIR, or external/vfx_x64)
VFX_DIR = os.environ.get('VFX_DIR') or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'addons', 'DLSS5NR01', 'external', 'vfx_x64')
NEEDS_VFX = {'vsr', 'vsr_4_3', 'vsr_move', 'vsr_nogate', 'vsr_fade', 'vsr_fade_nogate'}


def scenario_vsr(ctx, res, text, frame):
    res.check('the Video Super Resolution addon takes the NIS pass', 'DLSS REPLACED NIS' in text)
    res.check('no exception or failure in the VSR chain', 'raised exception' not in text and 'FAULT' not in text and 'VSRUPSC: cannot load' not in text and 'the VSR chain' not in text)
    m = re.search(r'VSR quality \d+, \d+x\d+ to \d+x\d+: ([0-9.]+) ms a frame', text)
    res.check('a frame costs under 8 ms on the render thread', m is not None and float(m.group(1)) < 8.0, (m.group(1) + ' ms') if m else 'no status line')


def scenario_vsr_move(ctx, res, text, frame):
    # the picture slides everywhere: the motion gate keeps NIS's picture (the test's fake NIS is magenta), VSR's is not shown
    res.check("on what moves the motion gate keeps NIS's picture", 'NIS KEPT' in text)
    res.check('no exception or failure in the VSR chain', 'raised exception' not in text and 'FAULT' not in text and 'the VSR chain' not in text)


def scenario_vsr_fade(ctx, res, text, frame):
    # a change of 2 levels a pass, far below the gate's threshold for one pass: the motion memory adds it up, and NIS's picture (the test's fake is magenta) is kept
    m = re.search(r"([0-9.]+)% of the output is the fake NIS pass's magenta", text)
    res.check("a slow change, added up over passes, counts as motion (NIS's picture kept)", m is not None and float(m.group(1)) > 80.0, (m.group(1) + ' % NIS') if m else 'no check-nis line')
    res.check('no exception or failure in the VSR chain', 'raised exception' not in text and 'FAULT' not in text and 'the VSR chain' not in text)


def scenario_vsr_fade_nogate(ctx, res, text, frame):
    m = re.search(r"([0-9.]+)% of the output is the fake NIS pass's magenta", text)
    res.check("with the gate off the slow change gets VSR's picture", m is not None and float(m.group(1)) < 1.0, (m.group(1) + ' % NIS') if m else 'no check-nis line')


def scenario_vsr_viewport(ctx, res, text, frame):
    scenario_vsr(ctx, res, text, frame)
    vp = re.search(r'\[check-vp\] .*?: (\d+) pixels of the borders lit', text)
    res.check("...and leaves Lossless Scaling's borders as they are", vp is not None and int(vp.group(1)) == 0, vp.group(0)[11:] if vp else 'no check-vp line')


SCENARIOS = [
    ('vsr', ['addon=VSRUPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'enabled=1', 'quality=1', 'vfxDir=' + VFX_DIR], scenario_vsr),
    ('vsr_move', ['addon=VSRUPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nismove=1', 'enabled=1', 'quality=1', 'vfxDir=' + VFX_DIR], scenario_vsr_move),
    ('vsr_nogate', ['addon=VSRUPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nismove=1', 'motionGate=0', 'enabled=1', 'quality=1', 'vfxDir=' + VFX_DIR], scenario_vsr),   # the gate off: VSR everywhere, also on what moves
    ('vsr_fade', ['addon=VSRUPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisfade=1', 'enabled=1', 'quality=1', 'vfxDir=' + VFX_DIR], scenario_vsr_fade),
    ('vsr_fade_nogate', ['addon=VSRUPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisfade=1', 'motionGate=0', 'enabled=1', 'quality=1', 'vfxDir=' + VFX_DIR], scenario_vsr_fade_nogate),
    ('vsr_4_3', ['addon=VSRUPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisvp=1', 'nisW=960', 'nisH=720', 'nisScale=1.5', 'enabled=1', 'quality=1', 'vfxDir=' + VFX_DIR], scenario_vsr_viewport),

    ('base', ['presentMode=0'], scenario_base),   # present mode off: with frame generation off the old result must go (present_mode tests it on)
    ('every2', ['presentMode=0', 'modelEvery=2'], scenario_every2),   # the setting Run the model: every 2nd real frame
    ('present_mode', ['offframes=150'], scenario_present_mode),   # frame generation off for 6 s: the model takes the presented frames
    ('present_wait', ['offframes=150', 'presentWait=1'], scenario_present_mode),   # the same, each frame waiting for its own result
    ('hdr_scrgb', ['offframes=150', 'hdr=scrgb'], scenario_hdr),   # ...then the presented frames HDR: 16-bit float scRGB
    ('hdr_pq', ['offframes=150', 'hdr=pq'], scenario_hdr),         # ...and HDR10 (10-bit PQ)
    ('record_replay', ['recordOn=1', 'recordSaveAfter=60', 'recordSeconds=3', 'recordFolder=@OUT@/recordings'], scenario_record_replay),   # save, read, play back
    ('hud_left_half', ['hud=0,0,0.5,1', 'hudFeather=0'], scenario_hud),
    ('sharpen', ['sharpen=0.8'], scenario_sharpen),
    ('shadows_up', ['shadows=1'], scenario_shadows_up),
    ('highlights_down', ['highlights=-1'], scenario_highlights_down),
    ('grain', ['grain=0.6', 'grainSize=1'], scenario_grain),
    ('grain_inside_hud', ['grain=0.6', 'hud=0,0,0.5,1', 'hudFeather=0'], scenario_grain_hud),
    ('smooth', ['deltaSmooth=0.5'], scenario_smooth),
    ('smooth_passes3', ['deltaSmooth=0.5', 'passes=3'], scenario_smooth_passes),
    ('ghost_off', ['flowsplit=1', 'ghostGuard=0'], scenario_ghost_off),
    ('ghost_on', ['flowsplit=1', 'ghostGuard=1'], scenario_ghost_on),
    ('ultrawide_bars', ['bars=1'], scenario_bars),   # the screen wider than the frame: the compose keeps to the picture (issue #13)
    ('flow_previous', ['freshFlow=0'], scenario_flow_previous),
    ('selftest', ['selfTestOnStart=1'], scenario_selftest),
    ('scaler', ['addon=DLSS4DLAA.dll', 'nis=1', 'sharpen=0'], scenario_scaler),   # no sharpening (the upscalers' default is 0.3): the baseline for scaler_sharp
    ('scaler_m', ['addon=DLSS4DLAA.dll', 'nis=1', 'dlaaPreset=13'], scenario_scaler),
    ('scaler_light', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisgen=1', 'sharpen=0.5', 'scalerLightGen=1'], scenario_scaler_light),   # lighter upscaling of generated frames (a test)
    ('scaler_sharp', ['addon=DLSS4DLAA.dll', 'nis=1', 'sharpen=0.5'], scenario_scaler),
    ('scaler_bgra', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1'], scenario_scaler_noflow),   # frame generation off: only NIS, on the BGRA8 capture
    ('scaler_move_none', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nismove=1', 'motionSource=2'], scenario_move_none),   # a sliding picture, DLSS told nothing moves
    ('scaler_move', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nismove=1'], scenario_move),
    ('fsr_aliased', ['addon=FSR3UPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisedge=1', 'sharpen=0'], scenario_fsr_aliased),   # a hard slanted edge
    ('fsr_edges', ['addon=FSR3UPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisedge=1', 'sharpen=0', 'scalerEdges=1'], scenario_fsr_edges),   # ...smoothed first
    ('fsr_edges_1x', ['addon=FSR3UPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisedge=1', 'sharpen=0', 'nisScale=1', 'scalerEdges=1'], scenario_fsr),   # (numbers only)
    ('fsr_edges_sharp', ['addon=FSR3UPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisedge=1', 'sharpen=1', 'sharpenScale=1.6', 'scalerEdges=1'], scenario_fsr_edges_sharp),   # edges, then our sharpening
    ('scaler_edges', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisedge=1', 'sharpen=0', 'scalerEdges=1'], scenario_scaler_edges),
    ('fsr_line', ['addon=FSR3UPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisline=1', 'sharpen=0'], scenario_fsr_line),   # a wire swaying in the wind
    ('fsr_line_stable', ['addon=FSR3UPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisline=1', 'sharpen=0', 'scalerStability=0.5'], scenario_fsr_line),
    ('scaler_hdr_scrgb', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nishdr=scrgb'], scenario_scaler_hdr),
    ('scaler_hdr_pq', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nishdr=pq'], scenario_scaler_hdr),
    ('fsr_hdr_scrgb', ['addon=FSR3UPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nishdr=scrgb'], scenario_scaler_hdr),
    ('scaler_not_nvidia', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'warp=1'], scenario_scaler_not_nvidia),
    ('scaler_4_3', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisvp=1', 'nisW=960', 'nisH=720', 'nisScale=1.5'], scenario_viewport),   # 4:3 on 16:9
    ('scaler_crop_3440', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisvp=1', 'nisW=1920', 'nisH=1081', 'nisScale=0.999'], scenario_viewport),   # issue 13: a 3440x1441 window drawn into 3438x1440 (a shrink of 0.06 %): taken as 1:1, the edges trimmed
    ('scaler_crop_3440_decoy', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisvp=1', 'nisW=1920', 'nisH=1081', 'nisScale=0.999', 'nisdecoy=1'], scenario_viewport_decoy),   # issue 13: two NIS-looking passes a frame
    ('scaler_crop_3440_decoy2', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisvp=1', 'nisW=1920', 'nisH=1081', 'nisScale=0.999', 'nisdecoy=2'], scenario_viewport_decoy),   # issue 13 again: the other pass gets a new constant buffer at every frame
    ('fsr_4_3', ['addon=FSR3UPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisvp=1', 'nisW=960', 'nisH=720', 'nisScale=1.5'], scenario_fsr_viewport),
    ('scaler_stable', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nismove=1', 'scalerStability=1'], scenario_stable),   # stability at 1 on the slide
    ('dlaa_4k_move', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nismove=1', 'nisW=3840', 'nisH=2160', 'nisScale=1'], scenario_move_4k),   # DLAA at 4K on the sliding picture: the estimate's cost
    ('fsr_scaler', ['addon=FSR3UPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1'], scenario_fsr),   # the FSR Upscaler, frame generation off
    ('fsr_move_none', ['addon=FSR3UPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nismove=1', 'motionSource=2'], scenario_fsr_move_none),
    ('fsr_move', ['addon=FSR3UPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nismove=1'], scenario_fsr_move),
    ('fsr_stable', ['addon=FSR3UPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nismove=1', 'scalerStability=1'], scenario_fsr_stable),
    ('xess_scaler', ['addon=XESSUPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1'], scenario_xess),   # the XeSS Upscaler, frame generation off
    ('xess_move_none', ['addon=XESSUPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nismove=1', 'motionSource=2'], scenario_xess_move_none),
    ('xess_move', ['addon=XESSUPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nismove=1'], scenario_xess_move),
    ('xess_hdr_scrgb', ['addon=XESSUPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nishdr=scrgb'], scenario_scaler_hdr),
    ('xess_4_3', ['addon=XESSUPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisvp=1', 'nisW=960', 'nisH=720', 'nisScale=1.5'], scenario_xess_viewport),
    ('scaler_tone', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'sharpen=0', 'brightness=0.15'], scenario_tone),   # brightness before the upscaler
    ('scaler_colour', ['addon=FSR3UPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'sharpen=0', 'saturation=0'], scenario_colour),   # colour before FSR
    ('scaler_tone_nr_on', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'sharpen=0', 'brightness=0.15', '_enabled=1'], scenario_tone),   # ...left to NR
    ('fsr_runtime_switch', ['addon=FSR3UPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisswitch=fsrRuntime=@FSR4@'], scenario_runtime_switch),   # the Runtimes list's +
    ('dlss_runtime_switch', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'nisswitch=dlssRuntime=@DLSSCOPY@'], scenario_runtime_switch),
    ('dlss_runtime_crashed', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'dlssRuntime=@DLSSCRASHED@'], scenario_runtime_crashed),   # a chosen runtime that did not finish its last start: the shipped one runs
    ('scaler_unload', ['addon=DLSS4DLAA.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'unload=1'], scenario_unload),   # switched off while running, then a new device
    ('fsr_unload', ['addon=FSR3UPSC.dll', 'nis=1', 'nisbgra=1', 'nisnoflow=1', 'unload=1'], scenario_unload),
    ('pair', ['second=DLSS4DLAA.dll'], scenario_pair),
    ('exit_abrupt', ['exitmode=abrupt'], scenario_none),   # the process ends with the addon loaded and no AddonShutdown, as Lossless Scaling does
    ('ui_shot', ['shot=@OUT@/ui_nr_panel.bmp', 'snapshotOnStart=1', 'hud=0,0,0.3,0.17/0.86,0,1,0.24', 'deltaSmooth=0.3', 'grain=0.2', 'shadows=0.2', 'presetNames=Night raid|Bright zone', 'preset.Night raid=shadows=0.4;grain=0.15', 'preset.Bright zone=highlights=-0.3;sharpen=0.2'], scenario_none),
]


# The everyday set (--quick): the frame reaching the model with its own motion, the older timing, an exit with no AddonShutdown, the DLSS 4
# Upscaler in place of NIS, and the two addons loaded together. The
# rest (looks, HUD, grain, smoothing, the self-test, the upscaler with preset M, the panel shot) run with no option, before a release.
QUICK = {'base', 'present_mode', 'hdr_scrgb', 'flow_previous', 'exit_abrupt', 'scaler', 'fsr_scaler', 'pair'}


# Retired (2026-09-26): kept working, but left out of every set; --only <name> or --retired runs them. Each one repeats another scenario's check,
# covers a finished feature nobody changes, or is not a test at all. Bring one back by taking it out of here.
RETIRED = {
    'present_wait':       'the same check as present_mode with one option more',
    'grain_inside_hud':   'grain and HUD protection, each has its own scenario',
    'smooth_passes3':     'smoothing with more passes; smooth covers the feature',
    'ghost_off':          'the ghosting guard: finished, not changed since',
    'ghost_on':           'the ghosting guard: finished, not changed since (needs ghost_off)',
    'scaler_m':           'the DLSS Upscaler with preset M: the same checks as scaler',
    'fsr_edges_1x':       'numbers only, no check of its own',
    'scaler_tone_nr_on':  'tone left to Neural Rendering: a corner of scaler_tone',
    'dlaa_4k_move':       'a cost measurement, not a pass / fail check',
    'ui_shot':            'renders the panel for the README, not a test',
}

SOLO = {'fsr_runtime_switch', 'dlss_runtime_switch', 'dlss_runtime_crashed'}   # run alone (see main)

# --changed: each scenario belongs to an area, and an area runs when a file it covers changed since the last commit. A change to what every
# addon of the pair shares (the addon's entry, settings, the test host, the build) runs everything; a change the list does not know runs QUICK.
def area_of(name):
    if name.startswith(('scaler', 'dlaa', 'dlss')): return 'dlss'
    if name.startswith('fsr'): return 'fsr'
    if name.startswith('xess'): return 'xess'
    if name == 'record_replay': return 'record'
    if name == 'selftest': return 'selftest'
    if name in ('pair', 'exit_abrupt', 'ui_shot'): return 'addon'
    return 'nr'


AREA_FILES = {   # regular expressions on the changed paths (forward slashes), under addons/DLSS5NR01/
    'nr':       r'src/engine/(nr_engine|nr_shaders|dlaa_model)|src/addon/(bridge|compose11|present_hook|frame_tap|tasks)',
    'dlss':     r'src/engine/(sr_engine|flow_estimator)|src/addon/scaler11',
    'fsr':      r'src/engine/(sr_engine|flow_estimator)|src/addon/scaler11|external/ffx',
    'xess':     r'src/engine/(sr_engine|flow_estimator)|src/addon/scaler11|external/xess',
    'record':   r'src/addon/(recorder|lsrec)|tools/lsrec_tool',
    'selftest': r'src/selftest/|src/addon/requirements',
    'addon':    r'src/addon/(panel|hud_editor|screenshot)',
}
EVERYTHING = r'src/addon/(addon|settings|state|runtime|log|product|hdr)\.|src/engine/hdr_hlsl|src/forwarder/|CMakeLists|tools/addon_host_test|^tools/run_hosttest_matrix'


def changed_scenarios():
    """The scenario names the files changed since the last commit call for (None: all of them)."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    git = lambda *a: subprocess.run(['git', '-C', root] + list(a), capture_output=True, text=True).stdout.split()
    changed = [p for p in git('diff', '--name-only', 'HEAD') + git('ls-files', '--others', '--exclude-standard') if not p.endswith('.md')]
    nr = [p[len('addons/DLSS5NR01/'):] if p.startswith('addons/DLSS5NR01/') else p for p in changed
          if p.startswith(('addons/DLSS5NR01/', 'tools/run_hosttest_matrix', 'manager/sdk/'))]
    if any(re.search(EVERYTHING, p) or p.startswith('manager/sdk/') for p in nr):
        return None
    areas = {a for a, rx in AREA_FILES.items() if any(re.search(rx, p) for p in nr)}
    names = {n for n, _, _ in SCENARIOS if area_of(n) in areas}
    if any(not any(re.search(rx, p) for rx in AREA_FILES.values()) for p in nr):
        names |= QUICK
    return names


def selftest_exe_checks(nr_dir, snippet):
    """Runs nr_selftest.exe directly: the real model must pass (exit 0), and each way of being unusable must end with its own code, never a crash."""
    exe = os.path.join(nr_dir, 'nr_selftest.exe')
    print('== nr_selftest.exe')
    if not os.path.exists(exe):
        print('  FAIL  nr_selftest.exe is not built'); return 1
    import tempfile
    tmp = tempfile.mkdtemp(prefix='nr_selftest_')
    garbage = os.path.join(tmp, 'garbage.dll'); open(garbage, 'wb').write(b'MZ' + os.urandom(25 * 1024 * 1024))
    ls = os.path.dirname(snippet)
    cases = [
        ('the real model passes', [exe, '--model', snippet, '--lsdir', ls], 0),
        ('a missing model file is MODEL_LOAD (14)', [exe, '--model', os.path.join(tmp, 'absent.dll')], 14),
        ('25 MB of random bytes is MODEL_LOAD (14)', [exe, '--model', garbage], 14),
        ('no NVIDIA card with that LUID is NO_GPU (10)', [exe, '--luid', '7f:1234', '--model', snippet], 10),
    ]
    bad = 0
    for what, cmd, want in cases:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
        lines = [l for l in p.stdout.splitlines() if l.startswith('SELFTEST ')]
        ok = p.returncode == want and bool(lines) and lines[-1].split()[1] == str(want)
        print('  %s  %s  (exit %s)' % ('PASS' if ok else 'FAIL', what, p.returncode)); bad += 0 if ok else 1

    # --report: a shareable text file, written whatever the result, that names the card, the driver and the model file's version and size, and never a folder
    for what, cmd, want in cases[:2]:
        rep = os.path.join(tmp, 'report_%d.txt' % want)
        p = subprocess.run(cmd + ['--report', rep], capture_output=True, text=True, timeout=180)
        text = open(rep, encoding='utf-8', errors='replace').read() if os.path.exists(rep) else ''
        row = [l for l in text.splitlines() if l.startswith('| ') and 'Card' not in l]
        checks = [
            ('is written', bool(text)),
            ('says the result', ('result:          %s' % ('PASS (code 0)' if want == 0 else 'MODEL_LOAD (code 14)')) in text),
            ('names the graphics card and a driver number', 'graphics card:' in text and 'NVIDIA driver:   ' in text and 'NVIDIA driver:   unknown' not in text),
            ('has the row for the compatibility table', len(row) == 1 and row[0].count('|') == 6 and ('PASS' if want == 0 else 'FAIL MODEL_LOAD') in row[0]),
            ('names no folder and no user name', chr(92) not in text and ':/' not in text and os.environ.get('USERNAME', '@@') not in text),
        ]
        if want == 0:
            checks.append(('gives the model file version and size', bool(__import__('re').search(r'model file:\s+nvngx_dlssnr\.dll, version [0-9.]+, [0-9.]+ MB', text))))
        for name, ok in checks:
            print('  %s  the report for "%s" %s' % ('PASS' if ok else 'FAIL', what, name)); bad += 0 if ok else 1
    return 1 if bad else 0


def panel_sections_closed_check():
    """Every collapsible section of the Neural Rendering panel must start closed: eam::ui::SectionHeader is called without its default-open argument."""
    import re
    src = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'addons', 'DLSS5NR01', 'src', 'addon', 'panel.cpp')
    text = open(src, encoding='utf-8', errors='replace').read()
    headers = re.findall(r'eam::ui::SectionHeader\(([^;{]*?)\)\)\s*\{', text)
    opens = [h for h in headers if re.search(r',\s*true\s*$', h.strip())]
    print('== panel sections')
    ok = len(headers) >= 6 and not opens
    print('  %s  all %d collapsible sections start closed%s' % ('PASS' if ok else 'FAIL', len(headers), '' if not opens else '  (open by default: %s)' % ', '.join(opens)))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nr', default=DEFAULT_NR)
    ap.add_argument('--snippet', default=DEFAULT_SNIPPET)
    # one folder per checkout: two sessions testing side by side (in two worktrees) must not mix their logs
    ap.add_argument('--out', default=os.path.join(os.environ.get('TEMP', '.'), 'hosttest_matrix_' + os.path.basename(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
    ap.add_argument('--only', default='')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--jobs', type=int, default=3, help='scenarios run side by side (1: one after another)')
    ap.add_argument('--changed', action='store_true', help='only the scenarios for the files changed since the last commit')
    ap.add_argument('--retired', action='store_true', help='also the retired scenarios (RETIRED)')
    ap.add_argument('--verbose', action='store_true', help='every check, not only the failing ones')
    ap.add_argument('--quick', action='store_true', help='only the everyday set: ' + ', '.join(sorted(QUICK)))
    a = ap.parse_args()
    if a.list:
        for n, k, _ in SCENARIOS:
            print(n, ' '.join(k))
        return 0
    os.makedirs(a.out, exist_ok=True)
    named = set(x for x in a.only.split(',') if x)
    only = named | (QUICK if a.quick else set())
    if a.changed:
        picked = changed_scenarios()
        if picked is not None and not picked:
            print('no model scenario covers what changed'); return 0
        only |= picked or set()
    ctx = {'pat': pattern(), 'nr': a.nr, 'snippet': a.snippet, 'out': a.out}
    t0 = time.time()
    todo = []
    crash_marker = {}
    try: os.remove(os.path.join(a.nr, 'runtime-trial.txt'))   # (one left by an earlier run)
    except OSError: pass
    for name, keys, checker in SCENARIOS:
        if only and name not in only and name != 'base':
            continue
        if name in NEEDS_VFX and not (os.path.exists(os.path.join(a.nr, 'VSRUPSC.dll')) and os.path.isdir(VFX_DIR)):
            if name in named: print('%s needs VSRUPSC.dll in %s and the Video Effects SDK in %s (VFX_DIR)' % (name, a.nr, VFX_DIR))
            continue
        if name in RETIRED and name not in named and not a.retired:
            continue
        keys = [k.replace('@OUT@', a.out.replace('\\', '/')) for k in keys]
        if any('@FSR4@' in k for k in keys):   # tools\fetch_fsr4.ps1 puts it there
            fsr4 = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'addons', 'DLSS5NR01', 'external', 'fsr4', 'amd_fidelityfx_dx12.dll')
            if not os.path.exists(fsr4):
                print('  skip  %s: run tools\\fetch_fsr4.ps1 first' % name); continue
            keys = [k.replace('@FSR4@', os.path.abspath(fsr4).replace('\\', '/')) for k in keys]
        if any('@DLSSCRASHED@' in k for k in keys):   # a copy of the DLSS runtime, chosen, with the marker a crash of it would have left
            src = os.path.join(a.nr, 'dlss', 'nvngx_dlss.dll')
            copy_dir = os.path.join(a.out, 'dlss_crashed'); os.makedirs(copy_dir, exist_ok=True)
            shutil.copyfile(src, os.path.join(copy_dir, 'nvngx_dlss.dll'))
            chosen = os.path.join(copy_dir, 'nvngx_dlss.dll').replace(chr(92), '/')
            crash_marker[name] = chosen   # written just before this scenario runs (the others, run side by side, would clear it: they have not chosen that runtime)
            keys = [k.replace('@DLSSCRASHED@', chosen) for k in keys]
        if any('@DLSSCOPY@' in k for k in keys):   # the shipped DLSS runtime, copied to a folder of its own (a second file to switch to)
            src = os.path.join(a.nr, 'dlss', 'nvngx_dlss.dll')
            copy_dir = os.path.join(a.out, 'dlss_copy'); os.makedirs(copy_dir, exist_ok=True)
            shutil.copyfile(src, os.path.join(copy_dir, 'nvngx_dlss.dll'))
            keys = [k.replace('@DLSSCOPY@', os.path.join(copy_dir, 'nvngx_dlss.dll').replace('\\', '/')) for k in keys]
        todo.append((name, keys, checker))

    # The hosts run side by side (--jobs at a time); the checks then run in the list's order, since some compare with an earlier scenario (base, ghost_off...).
    from concurrent.futures import ThreadPoolExecutor
    # SOLO scenarios restart an engine mid-run against a deadline, so they run alone, after the rest.
    run = lambda t: run_host(a.nr, a.snippet, t[1], a.out, t[0])
    with ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        done = dict(zip([t[0] for t in todo if t[0] not in SOLO], pool.map(run, [t for t in todo if t[0] not in SOLO])))
    for t in todo:
        if t[0] in SOLO:
            if t[0] in crash_marker:
                with open(os.path.join(a.nr, 'runtime-trial.txt'), 'wb') as marker: marker.write(crash_marker[t[0]].encode('utf-8'))
            done[t[0]] = run(t)
            if t[0] in crash_marker:
                try: os.remove(os.path.join(a.nr, 'runtime-trial.txt'))
                except OSError: pass
    runs = [done[t[0]] for t in todo]
    failed = 0
    for (name, keys, checker), (rc, text, frame, secs) in zip(todo, runs):
        res = Result()
        ctx['keys'] = keys
        basic(res, rc, text)
        if frame is None and name not in ('ui_shot', 'ultrawide_bars'):
            res.check('present_gen.bmp written', False)
        elif frame is not None or name == 'ultrawide_bars':
            try:
                checker(ctx, res, text, frame)
            except KeyError as e:   # it compares with a scenario that did not run
                res.check('needs %s to run first (add it to --only)' % e, False)
        if a.verbose or not res.ok:
            print('== %s  (%.0f s)  %s' % (name, secs, ' '.join(keys)))
            for ln in res.lines:
                if a.verbose or ln.startswith('FAIL'):
                    print('  ' + ln)
            if not res.ok:
                print('  (log: %s)' % os.path.join(a.out, name + '.log'))
        failed += 0 if res.ok else 1
    print('%d scenario(s) in %.0f s, %d at a time' % (len(todo), time.time() - t0, a.jobs))
    if not only or 'selftest' in only:
        failed += selftest_exe_checks(a.nr, a.snippet)
    if not only or 'ui_shot' in only:
        failed += panel_sections_closed_check()
    print('\n%s' % ('ALL SCENARIOS PASSED' if not failed else '%d SCENARIO(S) FAILED' % failed))
    print('logs and frames in', a.out)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
