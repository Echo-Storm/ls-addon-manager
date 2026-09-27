r"""A/B the upscalers (or Neural Rendering) on the test host: runs nr_hosttest once per variant and prints the picture-quality numbers the
host measures side by side, so a setting, a runtime or a code change can be judged by numbers rather than by eye. This is how the FSR flags,
OptiScaler's tuning and FSR 4 against FSR 3.1 were compared (2026-09-26: the flags made no measurable difference; OptiScaler's tuning was
worse on thin swaying lines, 10.44 against 8.76 levels; FSR 4 was better in motion, 11.14 against 14.22).

    python handoff\scripts\scaler_ab.py --base "addon=FSR3UPSC.dll nis=1 nisbgra=1 nisnoflow=1 nismove=1" ^
        --variant "stability 0: scalerStability=0" --variant "stability 0.5: scalerStability=0.5" [--repeat 2] [--snippet <model file>]

Each variant is "name: key=value key=value" on top of --base (the same keys the matrix uses: see tools\run_hosttest_matrix.py and
addons\DLSS5NR01\tools\addon_host_test.cpp). Scenes: nismove=1 (a sliding picture: [check-move]), nisline=1 (a thin swaying line:
[check-line]), nisedge=1 (a hard slanted edge: [check-edge]), nishdr=scrgb|pq ([check-nishdr]). Lower "off by" numbers are closer to
the true picture. It loads the GPU for about 20 seconds a run: not while a game runs. Needs the NVIDIA SDK build (build_all.ps1).
"""
import argparse
import os
import re
import subprocess
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))   # handoff/scripts -> the repository
NR_DIR = os.path.join(ROOT, 'addons', 'DLSS5NR01', 'build', 'Release')
LS_DIR = os.environ.get('LS_DIR', r'C:\Program Files (x86)\Steam\steamapps\common\Lossless Scaling')

# the host's measurements worth comparing: (label, pattern with one number group)
MEASURES = [
    ('move: off by', r'\[check-move\] the picture is off the moving picture by ([0-9.]+) levels'),
    ('line: off by', r'\[check-line\] near the swaying line the picture is off the true one by ([0-9.]+) levels'),
    ('edge: off by', r'\[check-edge\] the slanted edge is off the ideal smooth edge by ([0-9.]+) levels'),
    ('NIS pass: colour off by', r'\[check-nis\] .*?average colour off by ([0-9.]+) levels'),
    ('NIS pass: detail', r'\[check-nis\] .*?detail ([0-9.]+)'),
    ('GPU ms', r'upscaler: ([0-9.]+) ms a frame on the GPU at the end'),
]


def run(keys, snippet):
    exe = os.path.join(NR_DIR, 'nr_hosttest.exe')
    dll = os.path.join(NR_DIR, next((k[6:] for k in keys if k.startswith('addon=')), 'DLSS5NR01.dll'))
    rest = [k for k in keys if not k.startswith('addon=')]
    with tempfile.TemporaryDirectory() as work:
        proc = subprocess.run([exe, dll, '-', snippet] + rest, cwd=work, capture_output=True, text=True, errors='replace', timeout=300)
    return proc.stdout + proc.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--base', default='', help='keys every variant gets')
    ap.add_argument('--variant', action='append', required=True, help='"name: key=value ..."')
    ap.add_argument('--repeat', type=int, default=1, help='runs per variant (the numbers are averaged)')
    ap.add_argument('--snippet', default=os.path.join(LS_DIR, 'nvngx_dlssnr.dll'), help="the model file (Neural Rendering only; the person's own)")
    a = ap.parse_args()
    base = a.base.split()
    results = []
    for v in a.variant:
        name, _, keys = v.partition(':')
        sums, counts = {}, {}
        for _ in range(a.repeat):
            text = run(base + keys.split(), a.snippet)
            for label, pattern in MEASURES:
                m = re.search(pattern, text)
                if m:
                    sums[label] = sums.get(label, 0.0) + float(m.group(1)); counts[label] = counts.get(label, 0) + 1
        results.append((name.strip(), {k: sums[k] / counts[k] for k in sums}))
        print('done:', name.strip())
    labels = [label for label, _ in MEASURES if any(label in r for _, r in results)]
    width = max(len(n) for n, _ in results) + 2
    print('\n' + ''.ljust(width) + ''.join(label.rjust(26) for label in labels))
    for name, r in results:
        print(name.ljust(width) + ''.join((('%.3f' % r[label]) if label in r else '-').rjust(26) for label in labels))


if __name__ == '__main__':
    main()
