#!/usr/bin/env python3
"""Verify shipped binaries, source coverage, notices and every handoff checksum."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import tarfile
import zipfile

p = argparse.ArgumentParser(description=__doc__); p.add_argument('directory', type=Path); args = p.parse_args()
directory = args.directory

def sha(path):
    with path.open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()

checksums = {}
for line in (directory / 'SHA256SUMS').read_text().splitlines():
    digest, name = line.split('  ', 1)
    assert Path(name).name == name and name not in checksums, name
    assert sha(directory / name) == digest, name
    checksums[name] = digest
manifest = json.loads((directory / 'SOURCE-MANIFEST.json').read_text())
version = manifest['version']
apk_path = directory / f'GoblinReactor-{version}.apk'
assert sha(apk_path) == manifest['apk_sha256']
assert sha(directory / manifest['archive']) == manifest['archive_sha256']
assert (directory / 'SOURCE-REVISION').read_text().strip() == manifest['source_revision']
expected = {f['path']: f for f in manifest['files']}; assert len(expected) == len(manifest['files'])
with zipfile.ZipFile(apk_path) as apk:
    metadata = json.loads(apk.read('assets/release-info.json'))
    assert metadata['product_name'] == 'GoblinReactor' and metadata['version']['name'] == version
    for name, digest in metadata['source_inputs'].items():
        assert expected['project/' + name]['sha256'] == digest, name
    apk_notices = {name.removeprefix('assets/'): hashlib.sha256(apk.read(name)).hexdigest() for name in apk.namelist()
                   if name.startswith(('assets/uml-licenses/', 'assets/terminal-licenses/', 'assets/release-notices/')) and not name.endswith('/')}
with tarfile.open(directory / 'NOTICES.tar.gz') as archive:
    notices = {m.name: hashlib.file_digest(archive.extractfile(m), 'sha256').hexdigest() for m in archive if m.isfile()}
assert notices == apk_notices
with tarfile.open(directory / manifest['archive'], 'r|') as archive:
    for member in archive:
        assert member.isfile() and not PurePosixPath(member.name).is_absolute() and '..' not in PurePosixPath(member.name).parts
        entry = expected.pop(member.name)
        assert member.size == entry['size']
        assert hashlib.file_digest(archive.extractfile(member), 'sha256').hexdigest() == entry['sha256'], member.name
assert not expected, expected.keys()
for filename, key, artifact in [('release-artifact.json', 'apk_sha256', apk_path), ('bundle-artifact.json', 'aab_sha256', directory / f'GoblinReactor-{version}.aab')]:
    report = json.loads((directory / filename).read_text()); assert report['result'] == 'PASS' and report[key] == sha(artifact)
print(json.dumps({'result': 'PASS', 'version': version, 'source_revision': manifest['source_revision'],
                  'handoff_files': len(checksums), 'source_files': len(manifest['files']), 'notice_files': len(notices),
                  'checks': ['handoff SHA256SUMS', 'source archive member hashes', 'APK build inputs match committed source',
                             'APK notices match notice archive', 'artifact reports match shipped binaries']}, indent=2))
