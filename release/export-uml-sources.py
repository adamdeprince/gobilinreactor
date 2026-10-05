#!/usr/bin/env python3
"""Export pristine pinned UML dependencies from the local Git clones."""
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
output = root / 'release/build/sources/uml'; output.mkdir(parents=True, exist_ok=True)
items = []
for name, pin in json.loads((root / 'uml/sources.lock.json').read_text()).items():
    target = output / (name + '-' + pin['commit'] + '.tar.gz')
    if not target.exists():
        with target.open('wb') as raw, gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=0, compresslevel=6) as stream:
            process = subprocess.Popen(['git', '-C', str(root / 'uml/build' / name), 'archive', '--format=tar', '--prefix=' + name + '/', pin['commit']], stdout=subprocess.PIPE)
            shutil.copyfileobj(process.stdout, stream)
            if process.wait() != 0: raise SystemExit('git archive failed')
    with target.open('rb') as stream: digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    items.append({'path': 'uml/' + target.name, 'repository': pin['repository'], 'commit': pin['commit'], 'sha256': digest, 'size': target.stat().st_size})
# passt's patcher restores files with git show, so provide its pinned Git objects too.
bundle = output / 'passt.git.bundle'
if not bundle.exists():
    subprocess.run(['git', '-C', str(root / 'uml/build/passt'), 'bundle', 'create', str(bundle), 'HEAD'], check=True)
pin = json.loads((root / 'uml/sources.lock.json').read_text())['passt']
items.append({'path': 'uml/passt.git.bundle', 'repository': pin['repository'], 'commit': pin['commit'],
              'sha256': hashlib.sha256(bundle.read_bytes()).hexdigest(), 'size': bundle.stat().st_size})
lock = root / 'release/uml-archives.lock.json'
if lock.exists() and json.loads(lock.read_text()) != items: raise SystemExit('UML archive identity changed')
lock.write_text(json.dumps(items, indent=2) + '\n')
print('Pinned UML source exports verified')
