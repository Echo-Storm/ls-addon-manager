r"""Makes the manager's daily update check run at the next start: removes updates.last_check (and updates.skipped, a release declined with
"Don't ask again") from the addons' config.json, after a backup next to it. Lossless Scaling must be closed (it writes the file).

    python handoff\scripts\reset_update_check.py "<Lossless Scaling folder>\addons\config.json"
"""
import json
import shutil
import sys
import time

path = sys.argv[1]
shutil.copy2(path, path + '.bak-updatetest-' + time.strftime('%Y%m%d-%H%M%S'))
raw = open(path, 'rb').read()
bom = raw.startswith(b'\xef\xbb\xbf')
config = json.loads(raw.decode('utf-8-sig'))
updates = config.setdefault('global', {}).setdefault('updates', {})
before = dict(updates)
updates.pop('last_check', None)
updates.pop('skipped', None)
open(path, 'wb').write((b'\xef\xbb\xbf' if bom else b'') + json.dumps(config, indent=4).encode('utf-8'))
print('updates before:', before)
print('updates now:   ', updates)
