"""make_known_hashes.py: the list of every addon file the earlier releases shipped, for Setup's "do not replace a file the person has changed".

    python tools/make_known_hashes.py            # reads dist/<release>/ folders (Setup's --extract where there is a Setup, else the addons folder)
    python tools/make_known_hashes.py --add <folder holding an addons folder>    # adds one more (package.ps1 does this for each release it makes)

Writes installer/known-hashes.txt: one line a file, "<sha256> TAB <path under addons>". Setup packs it beside the files; an update replaces a file
of the addons folder only if it is what Setup recorded last time or appears here (a release's own file), so a file the person changed is left alone.
Only files that are in an addon folder count (not config.json).
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'installer', 'known-hashes.txt')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def lines_of(addons_dir):
    found = set()
    for base, _dirs, files in os.walk(addons_dir):
        for name in files:
            if name.lower() == 'config.json':
                continue
            full = os.path.join(base, name)
            rel = os.path.relpath(full, addons_dir).replace('/', '\\').lower()
            found.add((sha256(full), rel))
    return found


def load():
    found = set()
    if os.path.exists(OUT):
        for line in open(OUT, encoding='utf-8'):
            line = line.rstrip('\r\n')
            if len(line) > 65 and line[64] == '\t':
                found.add((line[:64], line[65:]))
    return found


def save(found):
    with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        for h, rel in sorted(found, key=lambda t: (t[1], t[0])):
            f.write(h + '\t' + rel + '\n')
    print(f'{len(found)} files in {OUT}')


def from_release_folder(folder):
    addons = os.path.join(folder, 'addons')
    if os.path.isdir(addons):
        return lines_of(addons)
    setup = os.path.join(folder, 'LSAddonManagerSetup.exe')
    if not os.path.exists(setup):
        return set()
    tmp = tempfile.mkdtemp(prefix='known_hashes_')
    try:
        subprocess.run([setup, '--extract', os.path.join(tmp, 'files')], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
        addons = os.path.join(tmp, 'files', 'addons')
        return lines_of(addons) if os.path.isdir(addons) else set()
    except Exception as e:  # an old Setup that cannot extract
        print('skipped', folder, e)
        return set()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    if len(sys.argv) > 2 and sys.argv[1] == '--add':
        found = load()
        found |= lines_of(os.path.join(sys.argv[2], 'addons'))
        save(found)
        return
    found = load()
    dist = os.path.join(ROOT, 'dist')
    for name in sorted(os.listdir(dist)):
        folder = os.path.join(dist, name)
        if os.path.isdir(folder) and name.endswith('-x64') and '1.0.1' not in name:
            got = from_release_folder(folder)
            print(name, len(got))
            found |= got
    save(found)


if __name__ == '__main__':
    main()
