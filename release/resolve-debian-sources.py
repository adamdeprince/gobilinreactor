#!/usr/bin/env python3
"""Resolve source identities from pinned .deb controls and Debian Sources indexes.

Download trixie, trixie-updates and trixie-security Sources.xz into build/cache
first. This maintainer step writes a lock; release collection uses that lock.
"""
import hashlib
import io
import json
import lzma
from pathlib import Path
import re
import subprocess
import tarfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

def paragraphs(text):
    for paragraph in text.split('\n\n'):
        fields = {}; key = None
        for line in paragraph.splitlines():
            if line.startswith((' ', '\t')) and key:
                fields[key] += '\n' + line
            elif ':' in line:
                key, value = line.split(':', 1); fields[key] = value.lstrip()
        if fields: yield fields

def main():
    index = {}
    for path in sorted((HERE / 'build/cache').glob('*-Sources.xz')):
        base = 'https://deb.debian.org/debian-security/' if 'security' in path.name else 'https://deb.debian.org/debian/'
        for entry in paragraphs(lzma.open(path, 'rt').read()):
            index[entry['Package'], entry['Version']] = (entry, base)
    binaries = []
    for name in ('debian-bootstrap.lock.json', 'debian-deployment.lock.json'):
        for binary in json.loads((ROOT / 'fixtures' / name).read_text()):
            archive = ROOT / 'fixtures/build' / Path(binary['Filename']).name
            assert hashlib.sha256(archive.read_bytes()).hexdigest() == binary['SHA256']
            member = next(n for n in subprocess.check_output(['ar', 't', archive], text=True).splitlines() if n.startswith('control.tar'))
            with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(['ar', 'p', archive, member]))) as tar:
                control = tar.extractfile(next(m for m in tar if m.name in ('control', './control'))).read().decode()
            fields = next(paragraphs(control))
            source = re.fullmatch(r'(\S+)(?: \(([^)]+)\))?', fields.get('Source', fields['Package']))
            binaries.append({'package': binary['Package'], 'version': binary['Version'],
                             'source': source[1], 'source_version': source[2] or binary['Version'],
                             'binary_sha256': binary['SHA256'], 'group': name})
    for binary in json.loads((HERE / 'boot-packages.lock.json').read_text())['packages']:
        binaries.append({**binary, 'group': 'initramfs'})
    sources = []
    for name, version in sorted({(b['source'], b['source_version']) for b in binaries}):
        if (name, version) not in index: raise SystemExit(f'Missing exact source index entry: {name} {version}')
        entry, base = index[name, version]
        files = []
        for line in entry['Checksums-Sha256'].splitlines():
            if not line.strip(): continue
            digest, size, filename = line.split()
            files.append({'path': 'debian/' + name + '/' + filename, 'url': base + entry['Directory'] + '/' + filename,
                          'sha256': digest, 'size': int(size)})
        sources.append({'name': name, 'version': version, 'files': files})
    (HERE / 'debian-sources.lock.json').write_text(json.dumps({'binaries': binaries, 'sources': sources}, indent=2) + '\n')
    print(f'{len(binaries)} binary records; {len(sources)} exact source packages')

if __name__ == '__main__': main()
