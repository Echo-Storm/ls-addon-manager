#!/usr/bin/env python
"""dev: one front door for building, testing, evaluating, deploying and inspecting this project, outside any chat session.

    python tools/dev.py status                  where things stand: git, builds against the sources, what is deployed, the last results
    python tools/dev.py logs [--addon FSR3UPSC] the last session of an addon's log, condensed to a dozen lines
    python tools/dev.py test [quick|changed|all|gpu|eval]    the suites (changed by default); every run is saved and only what changed is said
    python tools/dev.py gen [--quick] [--steps 1,3] [--update]   EchoFG against FSR and a blend on the recordings, against a saved baseline (uses the GPU)
    python tools/dev.py presenter               the frame generation presenter battery on a real swap chain (uses the GPU)
    python tools/dev.py build [--fg]            the manager and the addons (--fg: the EchoFG test build too)
    python tools/dev.py deploy [--fg]           into the Lossless Scaling folder (refuses while it or a game runs)
    python tools/dev.py rec [list|info FILE|export FILE OUTDIR [count=N]]   the recordings
    python tools/dev.py serve [--port N]        LS Project Manager: the board as a local page (move cards, add items, run the cheap checks); localhost only
    python tools/dev.py board [--html] [--open]  handoff/BOARD.md from handoff/board.json (statuses moved by commit trailers "Board: ID status"), the open issues, the last results
    python tools/dev.py map [DIR]               the folders; with a folder, what each file in it is (its header comment)
    python tools/dev.py find TERMS... [--budget N] [--page N] [--in code|docs|all]   ranked, token-budgeted search with stable handles (file:line)
    python tools/dev.py show FILE:LINE[-LINE]   those lines
    python tools/dev.py selftest                its own parsers

Every command prints a short answer (a dozen lines) and keeps the detail in .dev/ (results as JSON, full logs of what it ran). Settings: .dev/config.json
(all optional): ls_dir, recordings, bin, bin_fg, hosttest_snippet, eval_clips. Run from the main checkout (where the builds are), or from a worktree with
"bin" pointing at a build.
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

NOWIN = getattr(subprocess, 'CREATE_NO_WINDOW', 0)   # no console window flashing up when this runs without one (the desktop launcher)
HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
MAIN = ROOT
if '.claude' in ROOT.parts and 'worktrees' in ROOT.parts:   # a worktree: the builds (and the NVIDIA SDK they need) are in the main checkout
    MAIN = Path(*ROOT.parts[:ROOT.parts.index('.claude')])
DEV = MAIN / '.dev'
RESULTS = DEV / 'results'

DEFAULTS = {
    'ls_dir': os.environ.get('LS_DIR', r'D:\Utilities\Lossless Scaling'),
    'recordings': str(Path.home() / 'Videos' / 'Lossless Scaling'),
    'bin': str(MAIN / 'addons' / 'DLSS5NR01' / 'build' / 'Release'),
    'bin_fg': str(MAIN / 'addons' / 'DLSS5NR01' / 'build_fg' / 'Release'),
    'hosttest_snippet': r'D:\Utilities\Lossless Scaling\nvngx_dlssnr.dll',
    # the clips of the quality battery: name, file in the recordings folder, first frame, how many frames (42: 20 rebuilt frames; step=N keeps every Nth)
    'eval_clips': [
        {'name': 'fast', 'file': 'shf-win64-shipping_2026-09-27_04-10-25.lsrec'},
        {'name': 'calm1', 'file': 'shf-win64-shipping_2026-09-27_04-13-43.lsrec'},
        {'name': 'calm2', 'file': 'shf-win64-shipping_2026-09-27_04-15-29.lsrec'},
        {'name': 'wow4k-a', 'file': 'wowb_2026-09-27_23-54-07.lsrec', 'first': 20},
        {'name': 'wow4k-b', 'file': 'wowb_2026-09-27_23-54-14.lsrec', 'first': 20},
        {'name': 'sr2-a', 'file': 'Lossless Scaling_2026-10-11_02-32-14.lsrec'},   # the owner's Saints Row 2, fast camera pans (60-260 px a frame at 46 fps, 1080p middle of the 4K picture)
        {'name': 'sr2-b', 'file': 'Lossless Scaling_2026-10-11_02-32-36.lsrec'},
    ],
}


def config():
    c = dict(DEFAULTS)
    p = DEV / 'config.json'
    if p.exists():
        try:
            c.update(json.loads(p.read_text(encoding='utf-8')))
        except Exception as e:
            print('(.dev/config.json is not valid JSON: %s; the defaults are used)' % e)
    return c


# ---- running things ---------------------------------------------------------------------------------------------------------------------------------
def run(cmd, timeout=900, cwd=None, tag=None):
    """-> (returncode, output); the whole output is kept in .dev/runs/<tag>.log"""
    t0 = time.time()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout, cwd=str(cwd) if cwd else None, creationflags=NOWIN)
        rc, text = p.returncode, (p.stdout or '') + (p.stderr or '')
    except subprocess.TimeoutExpired as e:
        rc, text = -1, ((e.stdout or b'').decode('utf-8', 'replace') if isinstance(e.stdout, bytes) else (e.stdout or '')) + '\nTIMED OUT after %d s' % timeout
    except FileNotFoundError as e:
        rc, text = -2, 'not found: %s' % e
    if tag:
        d = DEV / 'runs'
        d.mkdir(parents=True, exist_ok=True)
        (d / (re.sub(r'[^A-Za-z0-9_.-]+', '_', tag) + '.log')).write_text(text, encoding='utf-8')
    return rc, text, time.time() - t0


def ps(script, *args, **kw):
    return run(['powershell', '-NoProfile', '-File', str(script)] + list(args), **kw)


def git(*args, where=None):
    return subprocess.run(['git', '-C', str(where or ROOT)] + list(args), capture_output=True, text=True, errors='replace', creationflags=NOWIN).stdout.strip()


def say(status, name, text=''):
    print('%-5s %-26s %s' % (status, name, text))


# ---- results: saved, and compared with the last run -------------------------------------------------------------------------------------------------
def load_latest():
    p = RESULTS / 'latest.json'
    if p.exists():
        try:
            return json.loads(p.read_text(encoding='utf-8'))
        except Exception:
            return {}
    return {}


def save_results(new):
    """new: {name: {'ok': bool, 'summary': str, 'value': any}}; merged into latest.json; prints what changed from the run before"""
    RESULTS.mkdir(parents=True, exist_ok=True)
    old = load_latest()
    changes = []
    for k, v in new.items():
        o = old.get(k)
        if o is None:
            changes.append('%s: new (%s)' % (k, 'ok' if v['ok'] else 'FAILED'))
        elif o.get('ok') != v['ok']:
            changes.append('%s: %s -> %s' % (k, 'ok' if o.get('ok') else 'FAILED', 'ok' if v['ok'] else 'FAILED'))
        elif o.get('summary') != v['summary'] and 'value' in v and isinstance(v['value'], (int, float)) and isinstance(o.get('value'), (int, float)) and abs(v['value'] - o['value']) >= 0.05:
            changes.append('%s: %.2f -> %.2f' % (k, o['value'], v['value']))
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    merged = dict(old)
    for k, v in new.items():
        v = dict(v); v['at'] = stamp
        merged[k] = v
    (RESULTS / 'latest.json').write_text(json.dumps(merged, indent=1), encoding='utf-8')
    (RESULTS / (stamp + '.json')).write_text(json.dumps(new, indent=1), encoding='utf-8')
    print('changed since the last run: %s' % ('; '.join(changes) if changes else 'nothing'))
    return changes


# ---- parsers (also what selftest checks) ------------------------------------------------------------------------------------------------------------
RE_AVG = re.compile(r'average over (\d+) rebuilt frames: \w+ ([\d.]+) dB, hold ([\d.]+) dB, blend ([\d.]+) dB')
RE_FGGEN = re.compile(r'(\d+) real frames ([\d.]+) ms apart, (\d+) made between \((\d+) by (.+?), the GPU ([\d.]+) ms each; shown ([\d.]+) ms after the real frame arrives; its present ([\d.]+) ms\); Lossless Scaling.s thread held ([\d.]+) ms a frame, back ([\d.]+) ms after')
RE_STALL = re.compile(r'stall monitor: no pass from Lossless Scaling for (\d+) ms')
RE_SLOWPIC = re.compile(r'a picture took (\d+) ms to finish')
RE_SQUARE = re.compile(r'square position: mean error ([\d.]+) px, worst ([\d.]+) px \((\d+) frames')
RE_GAP = re.compile(r'longest wait between the presenting thread.s calls into the presenter: (\d+) ms')


def parse_avg(text):
    m = RE_AVG.search(text)
    return (float(m.group(2)), float(m.group(4)), int(m.group(1))) if m else None   # (the generator, the blend, frames)


def parse_presenter(text):
    ok = 'FG PRESENT TEST PASSED' in text
    sq = RE_SQUARE.search(text); gap = RE_GAP.search(text)
    return {'ok': ok, 'mean': float(sq.group(1)) if sq else None, 'worst': float(sq.group(2)) if sq else None, 'gap': int(gap.group(1)) if gap else None}


def summarise_log(lines):
    """the last session of a log: counts and extremes"""
    start = 0
    for i, l in enumerate(lines):
        if ' initialised (host version' in l:
            start = i
    s = lines[start:]
    out = {'lines': len(s)}
    gen = [m for m in (RE_FGGEN.search(l) for l in s) if m]
    if gen:
        real = sum(int(m.group(1)) for m in gen); made = sum(int(m.group(3)) for m in gen)
        by = {}
        for m in gen:
            by[m.group(5)] = by.get(m.group(5), 0) + int(m.group(4))
        gpu = [float(m.group(6)) for m in gen if int(m.group(4)) > 0]; present = [float(m.group(8)) for m in gen]
        out['fg'] = {'real': real, 'made': made, 'by': by, 'interval_ms': round(sum(float(m.group(2)) for m in gen) / len(gen), 1),
                     'gpu_ms': (min(gpu), max(gpu)) if gpu else None, 'present_ms': (min(present), max(present)), 'held_ms': round(sum(float(m.group(9)) for m in gen) / len(gen), 1)}
    stalls = [int(m.group(1)) for m in (RE_STALL.search(l) for l in s) if m]
    out['stalls'] = (len(stalls), max(stalls)) if stalls else (0, 0)
    slow = [int(m.group(1)) for m in (RE_SLOWPIC.search(l) for l in s) if m]
    out['slow_pictures'] = (len(slow), max(slow)) if slow else (0, 0)
    out['engine_changes'] = [re.sub(r'^\[[\d:.]+\] ', '', l).strip() for l in s if 'made by ' in l and 'from now on' in l][-3:]
    out['problems'] = [l.strip()[:160] for l in s if re.search(r'exception 0x|FAULT|FAILED|could not|did not finish|crash', l, re.I) and 'repeat compare' not in l][-5:]
    out['recorder'] = [l.strip()[:150] for l in s if 'recorder:' in l][-2:]
    out['cadence'] = [l.strip()[:170] for l in s if 'source cadence' in l][-1:]
    out['display'] = [l.strip()[:200] for l in s if 'the display showed' in l][-1:]
    out['skip'] = [l.strip()[:150] for l in s if 'repeated pictures are not worked on again' in l][-1:]
    return out


# ---- commands -----------------------------------------------------------------------------------------------------------------------------------------
def cmd_status(a):
    c = config()
    print('git      %s %s, %s files changed, %s commits not on origin/main' % (git('rev-parse', '--abbrev-ref', 'HEAD'), git('rev-parse', '--short', 'HEAD'),
          len([l for l in git('status', '--short').splitlines() if l.strip()]), git('rev-list', '--count', 'origin/main..HEAD') or '?'))
    rc, text, _ = run(['gh', 'release', 'list', '--limit', '1', '--json', 'tagName,publishedAt'], timeout=30)
    try:
        rel = json.loads(text)[0]; print('release  %s (latest on GitHub, %s)' % (rel['tagName'], rel['publishedAt'][:10]))
    except Exception:
        tags = git('tag', '--sort=-v:refname').splitlines(); print('release  %s (latest local tag; gh not available)' % (tags[0] if tags else '?'))
    # the builds against the sources
    # (the builds are the main checkout's: older than its last commit that touched the sources, or than uncommitted source changes there)
    ct = git('log', '-1', '--format=%ct', '--', 'addons/DLSS5NR01/src', 'manager/src', 'addons/DLSS5NR01/CMakeLists.txt', where=MAIN)
    newest = float(ct) if ct else 0
    dirty = [l[3:] for l in git('status', '--short', '--', 'addons/DLSS5NR01/src', 'manager/src', where=MAIN).splitlines()]
    newest = max([newest] + [os.path.getmtime(MAIN / d) for d in dirty if (MAIN / d).exists()])
    b = Path(c['bin'])
    stale = []
    for name in ('FSR3UPSC.dll', 'DLSS5NR01.dll', 'DLSS4DLAA.dll', 'XESSUPSC.dll'):
        f = b / name
        if not f.exists():
            stale.append(name + ' (missing)')
        elif os.path.getmtime(f) < newest - 2:
            stale.append(name)
    mgr = MAIN / 'manager' / 'build' / 'Release' / 'Lossless.dll'
    if not mgr.exists() or os.path.getmtime(mgr) < newest - 2:
        stale.append('Lossless.dll')
    print('builds   %s' % ('up to date with the sources' if not stale else 'OLDER than the sources: ' + ', '.join(stale)))
    fg = Path(c['bin_fg']) / 'FSR3UPSC.dll'
    print('EchoFG   %s' % ('built (%s)' % datetime.datetime.fromtimestamp(os.path.getmtime(fg)).strftime('%m-%d %H:%M') if fg.exists() else 'no test build (tools\\build_framegen_test.ps1)'))
    # what is deployed
    ls = Path(c['ls_dir'])
    pairs = [(ls / 'Lossless.dll', mgr), (ls / 'addons' / 'FSR3UPSC' / 'FSR3UPSC.dll', fg if fg.exists() else b / 'FSR3UPSC.dll'),
             (ls / 'addons' / 'DLSS5NR01' / 'DLSS5NR01.dll', b / 'DLSS5NR01.dll'), (ls / 'addons' / 'DLSS4DLAA' / 'DLSS4DLAA.dll', b / 'DLSS4DLAA.dll'),
             (ls / 'addons' / 'XESSUPSC' / 'XESSUPSC.dll', b / 'XESSUPSC.dll')]
    diff = [d.name for d, s in pairs if d.exists() and s.exists() and d.stat().st_size != s.stat().st_size]
    print('deployed %s' % ('%s: matches the builds' % ls if ls.exists() and not diff else ('differs from the builds: ' + ', '.join(diff)) if ls.exists() else '%s not found' % ls))
    r = load_latest()
    if r:
        bad = [k for k, v in r.items() if not v.get('ok')]
        print('results  %d recorded, %s' % (len(r), ('FAILING: ' + ', '.join(bad)) if bad else 'all ok') + ' (last: %s)' % max(v.get('at', '') for v in r.values()))
    board = ROOT / 'handoff' / 'board.json'
    if board.exists():
        items = json.loads(board.read_text(encoding='utf-8')).get('items', [])
        counts = {}
        for i in items:
            counts[i.get('status', '?')] = counts.get(i.get('status', '?'), 0) + 1
        print('board    ' + ', '.join('%s %d' % (k, v) for k, v in sorted(counts.items())))


def cmd_logs(a):
    c = config()
    p = Path(c['ls_dir']) / 'logs' / (a.addon + '.log')
    if not p.exists():
        print('no %s' % p); return
    age = (time.time() - p.stat().st_mtime) / 3600
    s = summarise_log(p.read_text(encoding='utf-8', errors='replace').splitlines())
    print('%s: last written %.1f h ago, %d lines in its last session' % (p.name, age, s['lines']))
    if 'fg' in s:
        f = s['fg']
        print('frame generation: %d real frames (every %.0f ms), %d made: %s; GPU %s ms each, a present %s ms, the thread held %.0f ms a frame' % (
            f['real'], f['interval_ms'], f['made'], ', '.join('%d by %s' % (v, k) for k, v in f['by'].items()) or 'none by the engine', '%.1f-%.1f' % f['gpu_ms'] if f['gpu_ms'] else '-',
            '%.1f-%.1f' % f['present_ms'], f['held_ms']))
    else:
        print('frame generation: not in this session')
    print('stalls: %d (longest %d ms); slow pictures: %d (worst %d ms)' % (s['stalls'] + s['slow_pictures']))
    for k, label in (('engine_changes', 'engine'), ('cadence', 'cadence'), ('skip', 'repeat skip'), ('recorder', 'recorder'), ('display', 'display')):
        for l in s.get(k, []):
            print('%s: %s' % (label, l))
    for l in s['problems']:
        print('PROBLEM: %s' % l)
    mgr = Path(c['ls_dir']) / 'logs' / 'LSAddonManager.log'
    if mgr.exists():
        lines = mgr.read_text(encoding='utf-8', errors='replace').splitlines()
        starts = [i for i, l in enumerate(lines) if '[Core] Addon Manager for Lossless Scaling' in l and 'starting' in l]
        seg = lines[starts[-1]:] if starts else lines[-400:]
        loaded = sorted(set(re.findall(r"Loaded addon '([^']+)'", '\n'.join(seg))))
        applied = [re.sub(r'^\[[\d:.]+\] \[INFO\] \[Core\] ', '', l) for l in seg if 'ApplySettings' in l][-1:]
        print('manager session: addons loaded: %s' % (', '.join(loaded) or 'none'))
        for l in applied:
            print('Lossless Scaling: %s' % l[:200])


GEN_BASELINE = HERE / 'baselines' / 'gen.json'


def cmd_gen(a):
    c = config()
    exe = Path(c['bin']) / 'nr_fgeval.exe'
    if not exe.exists():
        print('no %s' % exe); return 2
    clips = c['eval_clips'][:3] if a.quick else c['eval_clips']
    steps = [int(x) for x in a.steps.split(',')]
    base = json.loads(GEN_BASELINE.read_text(encoding='utf-8')) if GEN_BASELINE.exists() else {}
    new, rows, flags = {}, [], []
    print('(the quality battery uses the GPU: %d clips x steps %s x FSR and EchoFG)' % (len(clips), steps))
    for clip in clips:
        rec = Path(c['recordings']) / clip['file']
        if not rec.exists():
            print('missing recording: %s' % rec); continue
        cells = []
        for step in steps:
            vals = {}
            for eng, extra in (('fsr', ['direct=1']), ('echofg', ['own=1', 'direct=1'])):
                out = DEV / 'gen_pictures' / clip['name']
                out.mkdir(parents=True, exist_ok=True)
                cmd = [str(exe), str(rec), str(out), 'count=%d' % (42 if step == 1 else 22 * step), 'step=%d' % step] + (['first=%d' % clip['first']] if 'first' in clip else []) + extra
                rc, text, secs = run(cmd, timeout=600, tag='gen_%s_x%d_%s' % (clip['name'], step, eng))
                r = parse_avg(text)
                if not r:
                    vals[eng] = None; flags.append('%s x%d %s: no result (rc %s)' % (clip['name'], step, eng, rc)); continue
                vals[eng] = r[0]; vals['blend'] = r[1]
                key = '%s/x%d/%s' % (clip['name'], step, eng)
                new[key] = r[0]
                if eng == 'echofg' and key in base and r[0] < base[key] - 0.1:
                    flags.append('REGRESSION %s: %.2f dB, was %.2f' % (key, r[0], base[key]))
                if eng == 'echofg' and key in base and r[0] > base[key] + 0.1:
                    flags.append('better %s: %.2f dB, was %.2f' % (key, r[0], base[key]))
            cells.append('x%d %s/%s (blend %s)' % (step, '%.1f' % vals['fsr'] if vals.get('fsr') else '-', '%.1f' % vals['echofg'] if vals.get('echofg') else '-', '%.1f' % vals['blend'] if vals.get('blend') else '-'))
        rows.append('%-8s %s' % (clip['name'], '   '.join(cells)))
    print('dB, FSR 3.1 / EchoFG:')
    for r in rows:
        print('  ' + r)
    for f in flags:
        print(('! ' if 'REGRESSION' in f else '  ') + f)
    if a.update:
        GEN_BASELINE.parent.mkdir(parents=True, exist_ok=True)
        base.update(new)
        GEN_BASELINE.write_text(json.dumps(base, indent=1, sort_keys=True), encoding='utf-8')
        print('baseline updated: %s (%d values)' % (GEN_BASELINE.relative_to(ROOT), len(base)))
    ok = not any('REGRESSION' in f or 'no result' in f for f in flags)
    save_results({'gen': {'ok': ok, 'summary': '; '.join(rows), 'value': sum(v for k, v in new.items() if k.endswith('/echofg')) / max(1, len([k for k in new if k.endswith('/echofg')]))}})
    return 0 if ok else 1


PRESENTER_SCENARIOS = [
    ('x2', ['mult=2']), ('x4', ['mult=4']), ('x8', ['mult=8', 'speed=8']), ('hdr x3', ['mult=3', 'hdr=1']), ('FSR x2', ['mult=2', 'own=0']),
    ('60fps source, dup', ['mult=4', 'fps=60', 'dup=2', 'speed=12']), ('multiplier switched', ['mult=4', 'switch=1', 'frames=45', 'speed=16']),
    ('resize', ['mult=4', 'resize=1', 'frames=200', 'speed=3']), ('EchoFG switched on', ['mult=2', 'toggle=1', 'frames=150', 'speed=4']),
]


def cmd_presenter(a):
    c = config()
    exe = Path(c['bin_fg']) / 'nr_fgpresent.exe'
    if not exe.exists():
        exe = Path(c['bin']) / 'nr_fgpresent.exe'
    if not exe.exists():
        print('no nr_fgpresent.exe (python tools/dev.py build --fg)'); return 2
    res, ok_all = {}, True
    for name, args in (PRESENTER_SCENARIOS[:3] if a.quick else PRESENTER_SCENARIOS):
        rc, text, secs = run([str(exe)] + args, timeout=300, tag='presenter_' + name, cwd=exe.parent)
        r = parse_presenter(text)
        ok_all &= r['ok']
        say('ok' if r['ok'] else 'FAIL', 'presenter ' + name, 'position error %s px (worst %s), longest wait %s ms, %.0f s' % (r['mean'], r['worst'], r['gap'], secs))
        res['presenter/' + name] = {'ok': r['ok'], 'summary': 'worst %s px, wait %s ms' % (r['worst'], r['gap']), 'value': r['worst'] if r['worst'] is not None else 0}
    save_results(res)
    return 0 if ok_all else 1


def cmd_test(a):
    tier = a.tier
    res, ok_all = {}, True
    if tier in ('quick', 'all'):
        rc, text, secs = ps(ROOT / 'tools' / 'ci.ps1', timeout=1800, tag='ci')
        ok = 'ALL CI TESTS PASSED' in text
        failed = re.findall(r'FAILED: (.+)', text)
        say('ok' if ok else 'FAIL', 'ci (no GPU)', '%.0f s%s' % (secs, '' if ok else ': ' + '; '.join(failed[:5])))
        res['ci'] = {'ok': ok, 'summary': 'ci %s' % ('passed' if ok else 'failed')}
        ok_all &= ok
    if tier in ('changed', 'all'):
        rc, text, secs = ps(ROOT / 'tools' / 'run_addon_tests.ps1', *(['-All'] if tier == 'all' else []), timeout=3600, tag='addon_tests')
        for m in re.finditer(r'^\s+(ok|FAIL\w*)\s+(.+?)\s+\((\d+) s\)', text, re.M):
            ok = m.group(1) == 'ok'
            say('ok' if ok else 'FAIL', m.group(2)[:26], '%s s' % m.group(3))
            res['suite/' + m.group(2)] = {'ok': ok, 'summary': m.group(1)}
            ok_all &= ok
        if 'Nothing changed that a test covers' in text:
            print('no suite is affected by the changes')
        elif 'all addon tests passed' not in text:
            ok_all = False; print('the addon tests did not all pass: see .dev/runs/addon_tests.log')
    if tier in ('gpu',):
        rc = cmd_presenter(argparse.Namespace(quick=False))
        ok_all &= rc == 0
    if tier in ('eval',):
        rc = cmd_gen(argparse.Namespace(quick=False, steps='1,3', update=False))
        ok_all &= rc == 0
    if res:
        save_results(res)
    print('RESULT: %s' % ('all ok' if ok_all else 'FAILURES'))
    return 0 if ok_all else 1


def cmd_build(a):
    errs = 0
    for name, cmd in (('manager', ['cmake', '--build', str(MAIN / 'manager' / 'build'), '--config', 'Release', '--', '/nologo', '/verbosity:minimal']),
                      ('addons', ['cmake', '--build', str(MAIN / 'addons' / 'DLSS5NR01' / 'build'), '--config', 'Release', '--', '/nologo', '/verbosity:minimal'])):
        rc, text, secs = run(cmd, timeout=1800, tag='build_' + name)
        e = [l for l in text.splitlines() if re.search(r'\berror\b', l, re.I)]
        errs += len(e) + (rc != 0)
        say('ok' if rc == 0 else 'FAIL', 'build ' + name, '%.0f s%s' % (secs, '' if rc == 0 else ': ' + (e[0][:150] if e else 'rc %d' % rc)))
    if a.fg:
        rc, text, secs = ps(ROOT / 'tools' / 'build_framegen_test.ps1', timeout=1800, tag='build_fg')
        say('ok' if rc == 0 else 'FAIL', 'build EchoFG', '%.0f s' % secs)
        errs += rc != 0
    return 1 if errs else 0


def running(names):
    rc, text, _ = run(['tasklist', '/fo', 'csv', '/nh'], timeout=30)
    low = text.lower()
    return [n for n in names if '"%s.exe"' % n.lower() in low]


def cmd_deploy(a):
    c = config()
    busy = running(['LosslessScaling', 'WowB', 'SaintsRow2', 'Tomb Raider', 'DOOMx64vk', 'SUPERHOT'])
    if busy:
        print('refusing: %s is running (close it and Lossless Scaling first)' % ', '.join(busy)); return 3
    rc, text, secs = ps(ROOT / 'tools' / 'deploy.ps1', '-What', 'all', '-LsDir', c['ls_dir'], timeout=600, tag='deploy')
    bad = [l for l in text.splitlines() if re.search(r'COULD NOT|not built|refus|running', l)]
    say('ok' if rc == 0 and not bad else 'FAIL', 'deploy all', '%d files%s' % (len(re.findall(r'^\[', text, re.M)), '' if not bad else ': ' + bad[0][:120]))
    if a.fg and rc == 0:
        rc2, text2, _ = ps(ROOT / 'tools' / 'deploy.ps1', '-What', 'fsr', '-BuildDir', 'build_fg', '-LsDir', c['ls_dir'], timeout=600, tag='deploy_fg')
        d = Path(c['ls_dir']) / 'addons' / 'FSR3UPSC' / 'FSR3UPSC.dll'; s = Path(c['bin_fg']) / 'FSR3UPSC.dll'
        same = d.exists() and s.exists() and d.stat().st_size == s.stat().st_size
        say('ok' if rc2 == 0 and same else 'FAIL', 'deploy EchoFG build', 'FSR3UPSC.dll %s' % ('matches build_fg' if same else 'DOES NOT match'))
        rc = rc2 if rc2 else (0 if same else 1)
    return rc


def cmd_rec(a):
    c = config()
    d = Path(c['recordings'])
    tool = Path(c['bin']) / 'nr_lsrec.exe'
    if a.what == 'list':
        files = sorted(d.glob('*.lsrec'), key=lambda p: p.stat().st_mtime, reverse=True)[:int(a.n)]
        for p in files:
            print('%s  %6.0f MB  %s' % (datetime.datetime.fromtimestamp(p.stat().st_mtime).strftime('%m-%d %H:%M'), p.stat().st_size / 1048576, p.name))
        print('(%d recordings in %s)' % (len(list(d.glob('*.lsrec'))), d))
    elif a.what == 'info':
        f = Path(a.args[0]); f = f if f.exists() else d / a.args[0]
        rc, text, _ = run([str(tool), 'info', str(f)], timeout=120)
        print(text.strip()[:1500])
    elif a.what == 'export':
        f = Path(a.args[0]); f = f if f.exists() else d / a.args[0]
        rc, text, _ = run([str(tool), 'export', str(f), a.args[1]] + a.args[2:], timeout=900, tag='rec_export')
        print('\n'.join(text.strip().splitlines()[-6:]))


def board_dump(board):
    """the board as JSON with one line per card, so a change reads as a one-line diff"""
    head = {k: v for k, v in board.items() if k != 'items'}
    lines = ['{'] + [' %s: %s,' % (json.dumps(k), json.dumps(v, ensure_ascii=False)) for k, v in head.items()]
    lines.append(' "items": [')
    lines.append((',' + '\n').join('  ' + json.dumps(i, ensure_ascii=False) for i in board.get('items', [])))
    lines += [' ]', '}']
    return '\n'.join(lines) + '\n'


def board_sync(board):
    """commits since the last render that carry a trailer line \"Board: ID status\" (several allowed) move those items; the head commit is remembered in the board"""
    head = git('rev-parse', 'HEAD')
    since = board.get('synced')
    rng = '%s..HEAD' % since if since and subprocess.run(['git', '-C', str(ROOT), 'cat-file', '-e', since], capture_output=True).returncode == 0 else '-n50'
    log = git('log', rng, '--reverse', '--format=%H%x1f%B%x1e') if rng != '-n50' else git('log', '-n50', '--reverse', '--format=%H%x1f%B%x1e')
    moved = []
    items = {i['id']: i for i in board.get('items', [])}
    if not since:
        board['synced'] = head
        return moved   # (the first render: nothing is replayed)
    for chunk in log.split('\x1e'):
        for m in re.finditer(r'^Board:\s*(\S+)\s+(doing|next|blocked|backlog|done)\b(.*)$', chunk, re.M | re.I):
            i = items.get(m.group(1))
            if i and i.get('status') != m.group(2).lower():
                moved.append('%s %s -> %s' % (i['id'], i.get('status'), m.group(2).lower())); i['status'] = m.group(2).lower()
                if m.group(3).strip():
                    i['note'] = m.group(3).strip()
    board['synced'] = head
    return moved


def render_board_html(board, results, issues, status_text, token=None):
    import html as H
    cols = [('doing', 'Doing'), ('next', 'Next'), ('blocked', 'Waiting'), ('backlog', 'Backlog'), ('done', 'Done')]
    items = board.get('items', [])
    areas = sorted({i.get('area', '') for i in items if i.get('area')})
    mine = [i for i in items if i.get('status') == 'blocked' and str(i.get('blocked_on', '')).lower().startswith('owner')]
    def card(i):
        w = ''
        if i.get('blocked_on'):
            who = 'you' if str(i['blocked_on']).lower().startswith('owner') else 'others'
            w = '<div class="wait %s">waiting on %s</div>' % (who, H.escape(str(i['blocked_on'])))
        n = '<div class="note">%s</div>' % H.escape(i['note']) if i.get('note') else ''
        wh = '<div class="where">%s</div>' % H.escape(i['where']) if i.get('where') else ''
        mv = ''
        if token:
            mv = '<select class="mv" data-id="%s" title="move this card">%s</select>' % (H.escape(i.get('id', '')), ''.join('<option value="%s"%s>%s</option>' % (k, ' selected' if k == i.get('status') else '', l) for k, l in cols))
        return ('<article class="card" data-area="%s" data-text="%s"><div class="top"><span class="id">%s</span><span class="chip a-%s">%s</span></div>'
                '<div class="title">%s</div>%s%s%s' + mv + '</article>') % (H.escape(i.get('area', '')), H.escape((i.get('id', '') + ' ' + i.get('title', '') + ' ' + i.get('note', '')).lower()),
                H.escape(i.get('id', '')), H.escape(i.get('area', '').lower()), H.escape(i.get('area', '')), H.escape(i.get('title', '')), w, n, wh)
    columns = ''
    for key, label in cols:
        group = [i for i in items if i.get('status') == key]
        columns += '<section class="col col-%s"><h2>%s <span class="count">%d</span></h2>%s</section>' % (key, label, len(group), ''.join(card(i) for i in group) or '<p class="empty">nothing</p>')
    res = ''.join('<li class="%s"><span>%s</span> %s</li>' % ('ok' if v.get('ok') else 'bad', H.escape(k), H.escape(str(v.get('summary', ''))[:90])) for k, v in sorted(results.items()))
    iss = ''.join('<li><span>#%d</span> %s <em>%s</em></li>' % (i['number'], H.escape(i['title'][:80]), H.escape(i['updatedAt'][:10])) for i in sorted(issues, key=lambda x: x['number']))
    youn = ''.join('<li><span>%s</span> %s <em>%s</em></li>' % (H.escape(i['id']), H.escape(i['title'][:90]), H.escape(str(i.get('blocked_on', ''))[:80])) for i in mine)
    chips = ''.join('<button class="f" data-a="%s">%s</button>' % (H.escape(a), H.escape(a)) for a in areas)
    return """<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>LS Project Manager</title>
<style>
:root{--bg:#f6f7f9;--panel:#fff;--ink:#1d2330;--mut:#6b7280;--line:#e3e6eb;--ok:#1f8f4e;--bad:#c0392b;--you:#b9560b;--chip:#e8ecf3}
@media (prefers-color-scheme:dark){:root{--bg:#12151b;--panel:#1b2029;--ink:#e6e9ef;--mut:#9aa3b2;--line:#2a3140;--ok:#4cc27f;--bad:#ef7b6e;--you:#f0a15c;--chip:#273044}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.45 system-ui,Segoe UI,sans-serif}
header{padding:18px 16px 6px;max-width:1500px;margin:auto}h1{margin:0 0 4px;font-size:22px}.sub{color:var(--mut);font-size:13px}
.strip{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:12px;max-width:1500px;margin:12px auto;padding:0 16px}
.box{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:12px 14px}.box h3{margin:0 0 8px;font-size:13px;text-transform:uppercase;letter-spacing:.05em;color:var(--mut)}
.box ul{list-style:none;margin:0;padding:0;font-size:13px}.box li{padding:3px 0;border-bottom:1px dashed var(--line)}.box li:last-child{border:0}.box li span{font-weight:600;margin-right:6px}.box li em{color:var(--mut);font-style:normal;margin-left:6px}
li.ok span{color:var(--ok)}li.bad span{color:var(--bad)}pre{margin:0;white-space:pre-wrap;font:12px/1.5 ui-monospace,Consolas,monospace}
.you{border-color:var(--you)}.you h3{color:var(--you)}
.tools{max-width:1500px;margin:6px auto;padding:0 16px;display:flex;gap:8px;flex-wrap:wrap;align-items:center}
.tools input{padding:7px 10px;border-radius:8px;border:1px solid var(--line);background:var(--panel);color:var(--ink);min-width:220px}
.f{border:1px solid var(--line);background:var(--chip);color:var(--ink);border-radius:999px;padding:4px 11px;cursor:pointer;font-size:12px}.f.on{background:var(--ink);color:var(--bg)}
.board{display:grid;grid-template-columns:repeat(5,minmax(230px,1fr));gap:12px;max-width:1500px;margin:12px auto 40px;padding:0 16px;align-items:start}
@media (max-width:1100px){.board{grid-template-columns:1fr}}
.col{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:10px}.col h2{margin:2px 4px 10px;font-size:15px}.count{color:var(--mut);font-weight:400;font-size:13px}
.col-doing{border-top:3px solid #3b82f6}.col-next{border-top:3px solid #8b5cf6}.col-blocked{border-top:3px solid #f59e0b}.col-backlog{border-top:3px solid #94a3b8}.col-done{border-top:3px solid var(--ok)}
.card{background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:8px 10px;margin-bottom:8px}.card.hide{display:none}
.top{display:flex;justify-content:space-between;align-items:center;margin-bottom:3px}.id{font:600 12px ui-monospace,Consolas,monospace;color:var(--mut)}
.chip{background:var(--chip);border-radius:999px;padding:1px 8px;font-size:11px}.title{font-weight:600}
.wait{margin-top:5px;font-size:12px;padding:3px 7px;border-radius:6px;background:var(--chip)}.wait.you{background:transparent;border:1px solid var(--you);color:var(--you)}
.live{margin:10px 0 0;display:flex;gap:8px;flex-wrap:wrap;align-items:center}.live button,.live input,.live select,.mv{padding:6px 10px;border-radius:8px;border:1px solid var(--line);background:var(--panel);color:var(--ink);font:inherit}.live button{cursor:pointer}.live button:disabled{opacity:.5}.mv{margin-top:6px;font-size:12px;padding:2px 6px}
#runout{flex-basis:100%;max-height:220px;overflow:auto;background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:8px;font:12px/1.5 ui-monospace,Consolas,monospace;white-space:pre-wrap;display:none}
.note,.where{margin-top:4px;color:var(--mut);font-size:12px}.where{font-family:ui-monospace,Consolas,monospace}.empty{color:var(--mut);margin:6px 4px}
</style></head><body>
<header><h1>LS Project Manager</h1><div class="sub">__SUB__</div>__LIVE__</header>
<div class="strip">
<div class="box you"><h3>Waiting on you (__N__)</h3><ul>__YOU__</ul></div>
<div class="box"><h3>Where things stand</h3><pre>__STATUS__</pre></div>
<div class="box"><h3>Last test results</h3><ul>__RES__</ul></div>
<div class="box"><h3>Open GitHub issues (__NI__)</h3><ul>__ISS__</ul></div>
</div>
<div class="tools"><input id="q" placeholder="search the cards"> __CHIPS__</div>
<main class="board">__COLUMNS__</main>
<script>
const TOKEN="__TOKEN__";
async function api(path,body){const r=await fetch(path,{method:body?'POST':'GET',headers:{'X-Token':TOKEN,'Content-Type':'application/json'},body:body?JSON.stringify(body):undefined});return r.json()}
document.querySelectorAll('.mv').forEach(s=>s.addEventListener('change',async()=>{const r=await api('/api/move',{id:s.dataset.id,status:s.value});if(r.ok)location.reload();else alert(r.error||'failed')}));
const addf=document.getElementById('addf');if(addf)addf.addEventListener('submit',async e=>{e.preventDefault();const f=new FormData(addf);const r=await api('/api/add',Object.fromEntries(f));if(r.ok)location.reload();else alert(r.error||'failed')});
const q=document.getElementById('quit');if(q)q.addEventListener('click',async()=>{await api('/api/quit',{});document.body.innerHTML='<p style="padding:24px">LS Project Manager has stopped. Start it again from the desktop icon.</p>'});
let polling=null;
async function poll(what){const r=await api('/api/run?what='+what);const o=document.getElementById('runout');o.style.display='block';o.textContent=r.text||'';o.scrollTop=o.scrollHeight;if(r.running){polling=setTimeout(()=>poll(what),1500)}else{document.querySelectorAll('.rb').forEach(b=>b.disabled=false);if(r.done)setTimeout(()=>location.reload(),1500)}}
document.querySelectorAll('.rb').forEach(b=>b.addEventListener('click',async()=>{const r=await api('/api/run',{what:b.dataset.w});if(!r.ok){alert(r.error||'failed');return}document.querySelectorAll('.rb').forEach(x=>x.disabled=true);poll(b.dataset.w)}));
const cards=[...document.querySelectorAll('.card')];let area=null;
function apply(){const q=document.getElementById('q').value.toLowerCase();cards.forEach(c=>c.classList.toggle('hide',!((!area||c.dataset.area===area)&&(!q||c.dataset.text.includes(q)))))}
document.getElementById('q').addEventListener('input',apply);
document.querySelectorAll('.f').forEach(b=>b.addEventListener('click',()=>{const on=b.classList.contains('on');document.querySelectorAll('.f').forEach(x=>x.classList.remove('on'));area=on?null:b.dataset.a;if(!on)b.classList.add('on');apply()}));
</script></body></html>""".replace('__SUB__', ('Live: moving a card or adding one writes handoff/board.json. Closing the terminal that runs <code>python tools/dev.py serve</code> stops it.' if token else 'Generated %s from handoff/board.json by <code>python tools/dev.py board --html</code>. Edit the JSON, or add a <code>Board: ID status</code> line to a commit message.' % datetime.datetime.now().strftime('%Y-%m-%d %H:%M'))).replace('__LIVE__', LIVE_BAR if token else '').replace('__TOKEN__', token or '').replace('__N__', str(len(mine))).replace('__YOU__', youn or '<li>nothing</li>').replace('__STATUS__', H.escape(status_text.strip())).replace('__RES__', res or '<li>no results yet</li>').replace('__NI__', str(len(issues))).replace('__ISS__', iss or '<li>none (or gh is not available)</li>').replace('__CHIPS__', chips).replace('__COLUMNS__', columns)


LIVE_BAR = """<form class="live" id="addf"><input name="title" placeholder="add a card: what is it" size="38" required><select name="status"><option>next</option><option>doing</option><option>blocked</option><option>backlog</option></select><input name="area" placeholder="area" size="9"><input name="blocked_on" placeholder="waiting on (if blocked)" size="22"><button>Add</button>
<span style="flex-basis:100%;height:0"></span>
<button type="button" class="rb" data-w="selftest">Self-test</button><button type="button" class="rb" data-w="test-quick">Quick tests</button><button type="button" class="rb" data-w="gen-quick">Frame generation scores</button><button type="button" class="rb" data-w="presenter-quick">Presenter check (uses the GPU)</button><button type="button" id="quit" style="margin-left:auto">Stop the server</button><div id="runout"></div></form>"""
RUNS = {'selftest': ['selftest'], 'test-quick': ['test', 'quick'], 'gen-quick': ['gen', '--quick', '--steps', '1,3'], 'presenter-quick': ['presenter', '--quick']}


def cmd_serve(a):
    import http.server, secrets, threading
    token = secrets.token_hex(16)
    bp = ROOT / 'handoff' / 'board.json'
    cache = {}
    lock = threading.Lock()
    run_state = {'proc': None, 'what': '', 'log': None, 'done': False}

    def cached(key, ttl, fn):
        now = time.time()
        if key not in cache or now - cache[key][0] > ttl:
            try:
                cache[key] = (now, fn())
            except Exception as e:
                cache[key] = (now, None)
        return cache[key][1]

    def issues():
        rc, text, _ = run(['gh', 'issue', 'list', '--state', 'open', '--limit', '30', '--json', 'number,title,updatedAt'], timeout=60)
        return json.loads(text) if rc == 0 else []

    def status_text():
        import io, contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            try:
                cmd_status(argparse.Namespace())
            except Exception as e:
                print('status: %s' % e)
        return buf.getvalue()

    def refresh_md():
        with contextlib.redirect_stdout(io.StringIO()):
            cmd_board(argparse.Namespace(html=False, open=False))

    import io, contextlib

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *x):
            pass

        def local(self):
            return self.headers.get('Host', '').split(':')[0] in ('127.0.0.1', 'localhost')

        def send(self, code, body, ctype='application/json'):
            b = body.encode('utf-8') if isinstance(body, str) else body
            self.send_response(code)
            self.send_header('Content-Type', ctype + '; charset=utf-8')
            self.send_header('Content-Length', str(len(b)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(b)

        def authed(self):
            return self.local() and self.headers.get('X-Token') == token

        def do_GET(self):
            if not self.local():
                return self.send(403, '{"error":"local only"}')
            path = self.path.split('?')[0]
            if path == '/':
                with lock:
                    board = json.loads(bp.read_text(encoding='utf-8'))
                    page = render_board_html(board, load_latest(), cached('iss', 300, issues) or [], cached('status', 120, status_text) or '', token)
                return self.send(200, page, 'text/html')
            if path == '/api/run':
                if not self.authed():
                    return self.send(403, '{"error":"no"}')
                what = self.path.split('what=')[-1]
                pr = run_state['proc']
                running = pr is not None and pr.poll() is None
                if pr is not None and not running:
                    run_state['done'] = True
                tail = ''
                if run_state['log'] and Path(run_state['log']).exists():
                    tail = Path(run_state['log']).read_text(encoding='utf-8', errors='replace')[-4000:]
                return self.send(200, json.dumps({'running': running, 'done': pr is not None and not running, 'text': tail}))
            self.send(404, '{"error":"not found"}')

        def do_POST(self):
            if not self.authed():
                return self.send(403, '{"ok":false,"error":"no"}')
            try:
                n = int(self.headers.get('Content-Length', '0'))
                data = json.loads(self.rfile.read(n) or b'{}')
            except Exception:
                return self.send(400, '{"ok":false,"error":"bad request"}')
            path = self.path.split('?')[0]
            with lock:
                if path in ('/api/move', '/api/add'):
                    board = json.loads(bp.read_text(encoding='utf-8'))
                    items = board.setdefault('items', [])
                    if path == '/api/move':
                        it = next((i for i in items if i.get('id') == data.get('id')), None)
                        if not it or data.get('status') not in ('doing', 'next', 'blocked', 'backlog', 'done'):
                            return self.send(400, '{"ok":false,"error":"no such card or status"}')
                        it['status'] = data['status']
                    else:
                        title = str(data.get('title', '')).strip()
                        if not title or data.get('status') not in ('doing', 'next', 'blocked', 'backlog'):
                            return self.send(400, '{"ok":false,"error":"a title and a status"}')
                        nums = [int(m.group(1)) for i in items for m in [re.match(r'^T(\d+)$', i.get('id', ''))] if m]
                        it = {'id': 'T%d' % (max(nums + [0]) + 1), 'title': title[:200], 'status': data['status']}
                        for k in ('area', 'blocked_on', 'note'):
                            if str(data.get(k, '')).strip():
                                it[k] = str(data[k]).strip()[:200]
                        items.append(it)
                    bp.write_text(board_dump(board), encoding='utf-8')
                    refresh_md()
                    return self.send(200, '{"ok":true}')
                if path == '/api/quit':
                    self.send(200, '{"ok":true}')
                    threading.Thread(target=srv.shutdown, daemon=True).start()
                    return
                if path == '/api/run':
                    what = data.get('what')
                    if what not in RUNS:
                        return self.send(400, '{"ok":false,"error":"unknown"}')
                    pr = run_state['proc']
                    if pr is not None and pr.poll() is None:
                        return self.send(409, '{"ok":false,"error":"one thing is running already"}')
                    DEV.mkdir(parents=True, exist_ok=True)
                    log = DEV / 'serve-run.log'
                    f = open(log, 'wb')
                    run_state.update(proc=subprocess.Popen([sys.executable, str(Path(__file__).resolve())] + RUNS[what], stdout=f, stderr=subprocess.STDOUT, cwd=str(ROOT), creationflags=NOWIN), what=what, log=str(log), done=False)
                    cache.pop('status', None)
                    return self.send(200, '{"ok":true}')
            self.send(404, '{"ok":false}')

    import urllib.request
    marker = DEV / 'serve.json'

    def ours(port):
        try:
            return 'LS Project Manager' in urllib.request.urlopen('http://127.0.0.1:%d/' % port, timeout=1).read().decode('utf-8', 'replace')
        except Exception:
            return False
    known = None
    try:
        known = json.loads(marker.read_text(encoding='utf-8')).get('port')
    except Exception:
        pass
    for port in ([known] if known else []) + [a.port]:   # already running (a second click on the icon): just show it
        if port and ours(port):
            if not a.no_open:
                os.startfile('http://127.0.0.1:%d/' % port)
            print('already running: http://127.0.0.1:%d/' % port)
            return
    srv = None
    for port in range(a.port, a.port + 20):   # the port may be in use by something else: take the next free one
        try:
            srv = http.server.ThreadingHTTPServer(('127.0.0.1', port), H)
            break
        except OSError:
            if ours(port):
                if not a.no_open:
                    os.startfile('http://127.0.0.1:%d/' % port)
                print('already running: http://127.0.0.1:%d/' % port)
                return
    if srv is None:
        print('no free port from %d to %d' % (a.port, a.port + 19)); return 1
    url = 'http://127.0.0.1:%d/' % port
    DEV.mkdir(parents=True, exist_ok=True)
    marker.write_text(json.dumps({'port': port}), encoding='utf-8')
    print('LS Project Manager: %s  (Ctrl+C to stop)' % url)
    if not a.no_open:
        try:
            os.startfile(url)
        except Exception:
            pass
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


def cmd_board(a):
    bp = ROOT / 'handoff' / 'board.json'
    if not bp.exists():
        print('no handoff/board.json'); return 2
    board = json.loads(bp.read_text(encoding='utf-8'))
    moved = board_sync(board)
    if moved:
        bp.write_text(board_dump(board), encoding='utf-8')
        print('moved by commits: ' + '; '.join(moved))
    order = ['doing', 'next', 'blocked', 'backlog', 'done']
    items = board.get('items', [])
    lines = ['# Project board', '', 'Generated by `python tools/dev.py board` from `handoff/board.json` (edit that, not this). Updated %s.' % datetime.date.today().isoformat(), '']
    for st in order:
        group = [i for i in items if i.get('status') == st]
        if not group:
            continue
        lines += ['## %s (%d)' % (st.capitalize(), len(group)), '']
        for i in group:
            extra = []
            if i.get('blocked_on'):
                extra.append('waiting on ' + i['blocked_on'])
            if i.get('area'):
                extra.append(i['area'])
            if i.get('where'):
                extra.append(i['where'])
            lines.append('- **%s** %s%s' % (i.get('id', ''), i.get('title', ''), (' (' + '; '.join(extra) + ')') if extra else ''))
            if i.get('note'):
                lines.append('  - ' + i['note'])
        lines.append('')
    r = load_latest()
    if r:
        lines += ['## Last test results', '']
        for k, v in sorted(r.items()):
            lines.append('- %s %s: %s (%s)' % ('ok' if v.get('ok') else '**FAILED**', k, v.get('summary', '')[:100], v.get('at', '')))
        lines.append('')
    iss = []
    rc, text, _ = run(['gh', 'issue', 'list', '--state', 'open', '--limit', '30', '--json', 'number,title,updatedAt'], timeout=60)
    if rc == 0:
        try:
            iss = json.loads(text)
            lines += ['## Open GitHub issues (%d)' % len(iss), '']
            lines += ['- #%d %s (updated %s)' % (i['number'], i['title'][:90], i['updatedAt'][:10]) for i in sorted(iss, key=lambda x: x['number'])]
            lines.append('')
        except Exception:
            pass
    (ROOT / 'handoff' / 'BOARD.md').write_text('\n'.join(lines), encoding='utf-8')
    if a.html or a.open:
        import io, contextlib
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            try:
                cmd_status(argparse.Namespace())
            except Exception as e:
                print('status: %s' % e)
        out = DEV / 'board.html'
        DEV.mkdir(parents=True, exist_ok=True)
        out.write_text(render_board_html(board, r, iss, buf.getvalue()), encoding='utf-8')
        print('%s' % out)
        if a.open:
            os.startfile(str(out))
    print('handoff/BOARD.md: %d items (%s)' % (len(items), ', '.join('%s %d' % (st, len([i for i in items if i.get('status') == st])) for st in order if any(i.get('status') == st for i in items))))


SRC_DIRS = ['addons/DLSS5NR01/src', 'addons/DLSS5NR01/tools', 'addons/DLSS5NR01/tests', 'manager/src', 'manager/sdk', 'manager/tools', 'installer', 'examples', 'tools', 'docs', 'handoff']
SRC_EXT = ('.cpp', '.h', '.hpp', '.py', '.ps1', '.md', '.json', '.cmake', '.txt')


def tracked(dirs=None):
    out = []
    for p in git('ls-files').splitlines():
        if p.endswith(SRC_EXT) and (not dirs or any(p.startswith(d + '/') or p == d for d in dirs)) and not p.startswith(('external/', 'third_party/')) and '/third_party/' not in p and '/external/' not in p:
            out.append(p)
    return out


def file_summary(path):
    """what the file says it is: the first lines of its header comment (or its title), cut short"""
    try:
        lines = (ROOT / path).read_text(encoding='utf-8', errors='replace').splitlines()[:40]
    except Exception:
        return ''
    text = []
    if path.endswith('.md'):
        for l in lines:
            if l.startswith('#'):
                return l.lstrip('# ').strip()[:150]
        return ''
    if path.endswith('.py'):
        joined = '\n'.join(lines)
        m = re.match(r'\s*(?:#!.*\n)?\s*(?:"""|\'\'\')(.*?)(?:"""|\'\'\')', joined, re.S)
        if m:
            return ' '.join(m.group(1).split())[:150]
    for l in lines:
        st = l.strip()
        if not st or st.startswith(('#pragma', '#include', '#define', '#ifndef', '#endif', '#if ', '#!', 'param(', '$', 'import ', 'from ', 'namespace ', 'using ', 'template')):
            if text: break
            continue
        is_c = path.endswith(('.cpp', '.h', '.hpp'))
        if st.startswith(('//', '/*', '*')) or (not is_c and st.startswith('#')):
            text.append(st.lstrip('/#* ').strip())
            if len(' '.join(text)) > 150: break
        else:
            break
    summary = ' '.join(' '.join(text).split())[:150]
    if not summary and path.endswith('.cpp'):   # no header of its own: its header's
        h = path[:-4] + '.h'
        if (ROOT / h).exists():
            hs = file_summary(h)
            return ('(implements) ' + hs)[:150] if hs else ''
    if not summary:
        names = re.findall(r'^\s*(?:class|struct|enum class|enum)\s+(\w+)', '\n'.join(lines), re.M)[:4]
        if names:
            return 'defines ' + ', '.join(names)
    return summary


def cmd_map(a):
    a_dir = bool(a.dir)
    dirs = [a.dir.rstrip('/')] if a.dir else None
    files = tracked(dirs)
    byDir = {}
    for f in files:
        byDir.setdefault(os.path.dirname(f), []).append(f)
    lines = []
    for d in sorted(byDir):
        lines.append('%s/' % d)
        for f in sorted(byDir[d]):
            lines.append('  %-30s %s' % (os.path.basename(f), file_summary(f)[:90 if not a_dir else 150]))
    if a.dir:
        print('\n'.join(lines[:400]))
        print('(%d files)' % len(files))
        return
    print('folders (python tools/dev.py map FOLDER lists the files of one):')
    for d in sorted(byDir):
        names = sorted(os.path.basename(f) for f in byDir[d])
        print('  %-38s %2d files: %s' % (d + '/', len(names), ', '.join(names[:4]) + (' ...' if len(names) > 4 else '')))


def cmd_find(a):
    terms = [x.lower() for x in a.terms]
    dirs = None if a.scope == 'all' else (['docs', 'handoff', 'tools/README.md'] if a.scope == 'docs' else [d for d in SRC_DIRS if d not in ('docs', 'handoff')])
    files = tracked(dirs)
    hits = {}
    pattern = '|'.join(re.escape(x) for x in terms)
    p = subprocess.run(['git', '-C', str(ROOT), 'grep', '-n', '-i', '-I', '-E', pattern, '--'] + files, capture_output=True, text=True, errors='replace')
    defre = re.compile(r'^\s*(class|struct|enum|namespace|def|void|bool|int|float|double|static|const|inline|auto|std::|[A-Z][A-Za-z0-9_:<>]*\s+[A-Za-z_][A-Za-z0-9_:]*\s*\()')
    for line in p.stdout.splitlines():
        m = re.match(r'^(.*?):(\d+):(.*)$', line)
        if not m:
            continue
        f, n, txt = m.group(1), int(m.group(2)), m.group(3)
        low = txt.lower()
        score = sum(1 for x in terms if x in low) + (2 if all(x in low for x in terms) else 0)
        if defre.match(txt) and any(x in low for x in terms):
            score += 3
        if txt.lstrip().startswith(('//', '#', '*')):
            score -= 0.5
        hits.setdefault(f, []).append((score, n, txt.strip()))
    ranked = []
    for f, hl in hits.items():
        coverage = sum(1 for x in terms if any(x in h[2].lower() for h in hl))   # how many of the terms the file has at all
        flat = f.lower().replace('_', '').replace('-', '')
        fs = coverage * 20 + sum(sorted((h[0] for h in hl), reverse=True)[:5]) + sum(8 for x in terms if x in flat)
        ranked.append((fs, f, sorted(hl, key=lambda h: -h[0])[:3]))
    ranked.sort(key=lambda r: -r[0])
    budget = a.budget * 4   # characters: about 4 to a token
    out, used, shown, skipped = [], 0, 0, 0
    page_start = (a.page - 1) * 10
    for i, (fs, f, hl) in enumerate(ranked):
        if i < page_start:
            continue
        block = ['%s  (%d lines match)' % (f, len(hits[f]))] + ['  %s:%d  %s' % (f, n, txt[:120]) for sc, n, txt in sorted(hl, key=lambda h: h[1])]
        size = sum(len(b) + 1 for b in block)
        if used + size > budget and shown:
            skipped = len(ranked) - i
            break
        out += block; used += size; shown += 1
    print('\n'.join(out) if out else 'nothing found for %s' % ' '.join(terms))
    if skipped:
        print('... %d more files: --page %d, or a longer --budget' % (skipped, a.page + 1))


def cmd_show(a):
    m = re.match(r'^(.*?):(\d+)(?:-(\d+))?$', a.handle)
    if not m:
        print('FILE:LINE or FILE:LINE-LINE'); return 2
    f, lo = m.group(1), int(m.group(2)); hi = int(m.group(3)) if m.group(3) else lo + 24
    lines = (ROOT / f).read_text(encoding='utf-8', errors='replace').splitlines()
    for i in range(max(1, lo), min(len(lines), hi) + 1):
        print('%5d  %s' % (i, lines[i - 1]))


def cmd_selftest(a):
    fails = 0
    def check(name, cond):
        nonlocal fails
        say('ok' if cond else 'FAIL', name); fails += not cond
    check('parse_avg', parse_avg('average over 20 rebuilt frames: FSR 24.46 dB, hold 18.94 dB, blend 20.50 dB; FSR the closest') == (24.46, 20.5, 20))
    check('parse_presenter', parse_presenter('x\nthe made frames\' square position: mean error 0.00 px, worst 0.01 px (69 frames; speed\nthe longest wait between the presenting thread\'s calls into the presenter: 47 ms\nFG PRESENT TEST PASSED') ==
          {'ok': True, 'mean': 0.0, 'worst': 0.01, 'gap': 47})
    sample = ['[1] FSR3UPSC initialised (host version 0x10200), addon dir x',
              '[23:54:53.695] frame generation: 374 real frames 35.3 ms apart, 504 made between (260 by EchoFG, the GPU 9.4 ms each; shown 10.0 ms after the real frame arrives; its present 7.18 ms); Lossless Scaling\'s thread held 19.3 ms a frame, back 7.5 ms after',
              '[23:55:00.291] stall monitor: no pass from Lossless Scaling for 3172 ms (normal ...',
              '[23:55:08.913] FSR upscaler: a picture took 55 ms to finish (the graphics card is held up']
    s = summarise_log(sample)
    check('summarise_log fg', s['fg']['real'] == 374 and s['fg']['by'] == {'EchoFG': 260} and s['fg']['gpu_ms'] == (9.4, 9.4))
    check('summarise_log stalls', s['stalls'] == (1, 3172) and s['slow_pictures'] == (1, 55))
    print('selftest: %s' % ('ok' if not fails else '%d failed' % fails))
    return 1 if fails else 0


def main():
    if sys.stdout is None or sys.stderr is None:   # started by pythonw (the desktop icon): no console, so a log
        DEV.mkdir(parents=True, exist_ok=True)
        sys.stdout = sys.stderr = open(DEV / 'serve.log', 'w', encoding='utf-8', buffering=1)
    ap = argparse.ArgumentParser(prog='dev.py', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('status').set_defaults(f=cmd_status)
    p = sub.add_parser('logs'); p.add_argument('--addon', default='FSR3UPSC'); p.set_defaults(f=cmd_logs)
    p = sub.add_parser('test'); p.add_argument('tier', nargs='?', default='changed', choices=['quick', 'changed', 'all', 'gpu', 'eval']); p.set_defaults(f=cmd_test)
    p = sub.add_parser('gen'); p.add_argument('--quick', action='store_true'); p.add_argument('--steps', default='1,3'); p.add_argument('--update', action='store_true'); p.set_defaults(f=cmd_gen)
    p = sub.add_parser('presenter'); p.add_argument('--quick', action='store_true'); p.set_defaults(f=cmd_presenter)
    p = sub.add_parser('build'); p.add_argument('--fg', action='store_true'); p.set_defaults(f=cmd_build)
    p = sub.add_parser('deploy'); p.add_argument('--fg', action='store_true'); p.set_defaults(f=cmd_deploy)
    p = sub.add_parser('rec'); p.add_argument('what', nargs='?', default='list', choices=['list', 'info', 'export']); p.add_argument('args', nargs='*'); p.add_argument('-n', default=8); p.set_defaults(f=cmd_rec)
    p = sub.add_parser('serve'); p.add_argument('--port', type=int, default=8761); p.add_argument('--no-open', action='store_true'); p.set_defaults(f=cmd_serve)
    p = sub.add_parser('board'); p.add_argument('--html', action='store_true', help='also .dev/board.html: the board as a page'); p.add_argument('--open', action='store_true', help='... and open it'); p.set_defaults(f=cmd_board)
    p = sub.add_parser('map'); p.add_argument('dir', nargs='?', default=''); p.set_defaults(f=cmd_map)
    p = sub.add_parser('find'); p.add_argument('terms', nargs='+'); p.add_argument('--budget', type=int, default=600); p.add_argument('--page', type=int, default=1); p.add_argument('--in', dest='scope', default='all', choices=['code', 'docs', 'all']); p.set_defaults(f=cmd_find)
    p = sub.add_parser('show'); p.add_argument('handle'); p.set_defaults(f=cmd_show)
    sub.add_parser('selftest').set_defaults(f=cmd_selftest)
    a = ap.parse_args()
    sys.exit(a.f(a) or 0)


if __name__ == '__main__':
    main()
