#!/usr/bin/env python3
"""Stage only the verified public release and download page for explicit rsync."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
version = json.loads((ROOT / 'harness/version.json').read_text())['name']
source = ROOT / 'dist' / version
subprocess.run(['python3', str(ROOT / 'tests/release_handoff_test.py'), str(source)], check=True)
output = ROOT / 'release/build/public/goblinreactor'
shutil.copytree(source, output / version, dirs_exist_ok=True)
shutil.copyfile(ROOT / 'harness/res/drawable-nodpi/goblin_logo.png', output / 'goblin.png')
apk = source / f'GoblinReactor-{version}.apk'
archive = source / f'GoblinReactor-{version}-corresponding-source.tar'
replacements = {'VERSION': version, 'SIZE': str(round(apk.stat().st_size / 2**20)),
                'SOURCE_SIZE': str(round(archive.stat().st_size / 2**20)),
                'SHA256': hashlib.sha256(apk.read_bytes()).hexdigest()}
page = (ROOT / 'release/preview-index.html').read_text()
for name, value in replacements.items(): page = page.replace('@@' + name + '@@', value)
assert '@@' not in page
(output / 'index.html').write_text(page)
print(output)
