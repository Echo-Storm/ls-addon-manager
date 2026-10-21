"""board_note.py ID [--status S] [--note TEXT]: sets a card's status and/or puts a note in front of its note in handoff/board.json (the lines keep their layout).

    python tools/board_note.py E6 --status backlog --note "2026-10-20: ..."
"""
import argparse
import json
import os

PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'handoff', 'board.json')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('id'); ap.add_argument('--status'); ap.add_argument('--note')
    a = ap.parse_args()
    raw = open(PATH, encoding='utf-8', newline='').read()
    eol = '\r\n' if '\r\n' in raw else '\n'
    lines = raw.split(eol)
    for n, line in enumerate(lines):
        t = line.strip().rstrip(',')
        if not t.startswith('{"id"'):
            continue
        try:
            d = json.loads(t)
        except ValueError:
            continue
        if d['id'] != a.id:
            continue
        if a.status:
            if a.status == 'done' and d.get('status') != 'done':
                import datetime
                d['doneAt'] = datetime.date.today().isoformat()   # (the board shows the newest 12 done cards; the rest are its archive)
            elif a.status != 'done':
                d.pop('doneAt', None)
            d['status'] = a.status
        if a.note:
            d['note'] = a.note + (' ' + d['note'] if d.get('note') else '')
        indent = line[:len(line) - len(line.lstrip())]
        comma = ',' if line.rstrip().endswith(',') else ''
        lines[n] = indent + json.dumps(d, ensure_ascii=False) + comma
        open(PATH, 'w', encoding='utf-8', newline='').write(eol.join(lines))
        print('updated', a.id, d['status'])
        return
    raise SystemExit('no card ' + a.id)


main()
