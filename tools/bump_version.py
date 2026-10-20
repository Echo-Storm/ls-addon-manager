"""bump_version.py NEW_VERSION [--apply] [--date YYYY-MM-DD]: every place that holds the release's number, in one go. Without --apply it only says what it would change.

    python tools/bump_version.py 1.0.0                 what would change (nothing is written)
    python tools/bump_version.py 1.0.0 --apply        write it (then: git diff, run tools\\ci.ps1, commit; the tag and the release are the owner's)

What it changes: manager/CMakeLists.txt (project VERSION), manager/sdk/include/eam/version.h (EAM_VERSION_STRING), the six addon.json files, kVersion in echofg_addon.cpp, the number in
README.md's header line, and CHANGELOG.md ("## Unreleased" becomes "## NEW_VERSION (date)" with a fresh empty "## Unreleased" above it). What it does NOT change, on purpose: the
addon API's own number (it moves with the interface, not the release), the "New in" paragraph of README.md and the status of ROADMAP.md (words, written by hand: see handoff/release-1.0.md),
and the places that name 0.10.0 as an example or as history (the update and installer tests compare versions as numbers; comments say "up to 0.10.0").
It refuses when the current number is not found where it expects it, rather than guess.
"""
import datetime
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def current():
    text = (ROOT / 'manager/sdk/include/eam/version.h').read_text(encoding='utf-8')
    m = re.search(r'#define EAM_VERSION_STRING "([^"]+)"', text)
    if not m:
        sys.exit('version.h: no EAM_VERSION_STRING')
    return m.group(1)


def read(path):
    raw = (ROOT / path).read_bytes()
    return raw.decode('utf-8-sig'), raw.startswith(b'\xef\xbb\xbf'), b'\r\n' in raw


def write(path, text, bom, crlf):
    text = text.replace('\r\n', '\n')
    if crlf:
        text = text.replace('\n', '\r\n')
    (ROOT / path).write_bytes((b'\xef\xbb\xbf' if bom else b'') + text.encode('utf-8'))


def main():
    argv = sys.argv[1:]
    if '--date' in argv:
        i = argv.index('--date'); del argv[i:i + 2]
    args = [a for a in argv if not a.startswith('--')]
    if len(args) != 1:
        sys.exit(__doc__)
    new = args[0]
    apply_ = '--apply' in sys.argv
    date = sys.argv[sys.argv.index('--date') + 1] if '--date' in sys.argv else datetime.date.today().isoformat()
    old = current()
    if old == new:
        sys.exit('already %s' % new)
    esc = re.escape(old)
    edits = []   # (path, pattern, replacement, expected count)
    edits.append(('manager/CMakeLists.txt', r'(project\(LSAddonManager VERSION )%s(\))' % esc, r'\g<1>%s\g<2>' % new, 1))
    edits.append(('manager/sdk/include/eam/version.h', r'(#define EAM_VERSION_STRING ")%s(")' % esc, r'\g<1>%s\g<2>' % new, 1))
    for addon in sorted((ROOT / 'addons/DLSS5NR01').glob('addon.json')) + sorted((ROOT / 'addons/DLSS5NR01/products').glob('*/addon.json')):
        edits.append((str(addon.relative_to(ROOT)).replace('\\', '/'), r'("version":\s*")%s(")' % esc, r'\g<1>%s\g<2>' % new, 1))
    edits.append(('addons/DLSS5NR01/src/echofg/echofg_addon.cpp', r'(kVersion = ")%s(")' % esc, r'\g<1>%s\g<2>' % new, 1))
    edits.append(('README.md', r'(&nbsp; \*\*)%s(\*\*)' % esc, r'\g<1>%s\g<2>' % new, 1))
    problems = []
    changes = []
    for path, pat, rep, count in edits:
        text, bom, crlf = read(path)
        out, n = re.subn(pat, rep, text)
        if n != count:
            problems.append('%s: expected %d place(s) with %s, found %d' % (path, count, old, n))
            continue
        changes.append((path, out, bom, crlf))
    text, bom, crlf = read('CHANGELOG.md')
    if '## Unreleased' not in text:
        problems.append('CHANGELOG.md: no "## Unreleased" heading')
    else:
        changes.append(('CHANGELOG.md', text.replace('## Unreleased', '## Unreleased\n\n## %s (%s)' % (new, date), 1), bom, crlf))
    if problems:
        print('\n'.join(problems))
        sys.exit('nothing written')
    for path, *_ in changes:
        print('%s %s -> %s: %s' % ('changed' if apply_ else 'would change', old, new, path))
    if apply_:
        for path, out, bom, crlf in changes:
            write(path, out, bom, crlf)
        print('\nwritten. Next: git diff --stat; powershell -File tools\\ci.ps1; commit. The README "New in" paragraph and ROADMAP.md are written by hand (handoff/release-1.0.md).')


main()
