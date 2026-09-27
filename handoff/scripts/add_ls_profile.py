r"""Adds a Lossless Scaling game profile by copying an existing one, set for testing the upscalers: NIS, HDR support on, frame generation off,
auto-scale after the copied delay. Settings.xml is backed up next to itself first. Lossless Scaling must be closed (it writes the file).
Used 2026-09-26 for metro.exe and SHf-Win64-Shipping.exe (Silent Hill f).

    python handoff\scripts\add_ls_profile.py <new title, e.g. game.exe> "<full path of the game's exe>" [--from <existing profile title>] [--no-hdr]
"""
import argparse
import os
import re
import shutil
import time

ap = argparse.ArgumentParser()
ap.add_argument('title')
ap.add_argument('exe')
ap.add_argument('--from', dest='source', default=None, help='the profile to copy (default: the first after Default)')
ap.add_argument('--no-hdr', action='store_true')
a = ap.parse_args()

xml = os.path.expandvars(r'%LOCALAPPDATA%\Lossless Scaling\Settings.xml')
shutil.copy2(xml, xml + '.bak-profile-' + time.strftime('%Y%m%d-%H%M%S'))
raw = open(xml, 'rb').read()
bom = raw.startswith(b'\xef\xbb\xbf')
s = raw.decode('utf-8-sig')
crlf = '\r\n' in s
s = s.replace('\r\n', '\n')
if '<Title>' + a.title + '</Title>' in s:
    raise SystemExit('a profile with that title exists already')
profiles = re.findall(r'    <Profile>\n.*?    </Profile>\n', s, re.S)
source = next((p for p in profiles if a.source and '<Title>' + a.source + '</Title>' in p), None) or next(p for p in profiles if '<Title>Default</Title>' not in p)
p = re.sub(r'<Title>.*?</Title>', lambda m: '<Title>' + a.title + '</Title>', source, count=1)
if '<Path>' in p:
    p = re.sub(r'<Path>.*?</Path>', lambda m: '<Path>' + a.exe + '</Path>', p, count=1)
else:
    p = p.replace('</Title>\n', '</Title>\n      <Path>' + a.exe + '</Path>\n', 1)
p = re.sub(r'<ScalingType>.*?</ScalingType>', '<ScalingType>NIS</ScalingType>', p)
p = re.sub(r'<FrameGeneration>.*?</FrameGeneration>', '<FrameGeneration>Off</FrameGeneration>', p)
p = re.sub(r'<AutoScale>.*?</AutoScale>', '<AutoScale>true</AutoScale>', p)
p = re.sub(r'<HdrSupport>.*?</HdrSupport>', '<HdrSupport>' + ('false' if a.no_hdr else 'true') + '</HdrSupport>', p)
at = s.index('  </GameProfiles>')
s = s[:at] + p + s[at:]
if crlf:
    s = s.replace('\n', '\r\n')
open(xml, 'wb').write((b'\xef\xbb\xbf' if bom else b'') + s.encode('utf-8'))
print('added the profile', a.title)
