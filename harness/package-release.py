#!/usr/bin/env python3
"""Collect signed internal-testing artifacts without keys or test components."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tarfile
import zipfile

root = Path(__file__).resolve().parents[1]
version = json.loads((root / 'harness/version.json').read_text())['name']
assert re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+(?:-[a-z0-9.-]+)?', version)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--validation', type=Path, default=root / f'harness/results/release-{version}/README.md')
args = parser.parse_args()
validation_source = args.validation.resolve()
assert validation_source.is_file(), f'Record this release\'s validation in {validation_source}'
apk = root / 'harness/build-release/goblin-sentry.apk'
bundle = root / 'harness/build-bundle/goblin.aab'
build = json.loads((root / 'harness/build-bundle/build.json').read_text())
assert build['source_apk_sha256'] == hashlib.sha256(apk.read_bytes()).hexdigest(), 'Rebuild the bundle from the current APK'
assert build['aab_sha256'] == hashlib.sha256(bundle.read_bytes()).hexdigest(), 'Bundle identity changed'
with zipfile.ZipFile(apk) as archive:
    metadata = json.loads(archive.read('assets/release-info.json'))
    assert metadata['version'] == json.loads((root / 'harness/version.json').read_text())
    assert metadata['product_name'] == 'GoblinReactor'
for name, artifact, key in [('release-artifact.json', apk, 'apk_sha256'), ('bundle-artifact.json', bundle, 'aab_sha256')]:
    report = json.loads((validation_source.parent / name).read_text())
    assert report['result'] == 'PASS' and report[key] == hashlib.sha256(artifact.read_bytes()).hexdigest(), 'Stale validation: ' + name
directory = root / 'dist' / version
directory.mkdir(parents=True, exist_ok=True)
source_manifest = json.loads((directory / 'SOURCE-MANIFEST.json').read_text())
assert source_manifest['version'] == version
assert source_manifest['apk_sha256'] == hashlib.sha256(apk.read_bytes()).hexdigest(), 'Stale corresponding source'
source_archive = directory / source_manifest['archive']
with source_archive.open('rb') as stream:
    assert hashlib.file_digest(stream, 'sha256').hexdigest() == source_manifest['archive_sha256']
assert (directory / 'SOURCE-REVISION').read_text().strip() == source_manifest['source_revision']
# Export notices from the exact APK, keeping their original hierarchy.
with zipfile.ZipFile(apk) as archive, tarfile.open(directory / 'NOTICES.tar.gz', 'w:gz') as notices:
    for name in sorted(archive.namelist()):
        if not name.startswith(('assets/uml-licenses/', 'assets/terminal-licenses/', 'assets/release-notices/')) or name.endswith('/'): continue
        import io
        data = archive.read(name)
        member = tarfile.TarInfo(name.removeprefix('assets/')); member.size = len(data); member.mode = 0o644
        notices.addfile(member, io.BytesIO(data))
files = {
    f'GoblinReactor-{version}.apk': apk,
    f'GoblinReactor-{version}.aab': bundle,
    'TESTING.md': root / 'docs/internal-testing.md',
    'TRADEMARK.md': root / 'docs/trademark.md',
    'VALIDATION.md': validation_source,
    'RELEASE_NOTES.md': root / 'release/RELEASE_NOTES.md',
    'BUILDING.md': root / 'release/README.md',
    'LICENSE': root / 'LICENSE',
    'COPYING': root / 'COPYING',
    'THIRD_PARTY_NOTICES.md': root / 'THIRD_PARTY_NOTICES.md',
    'release-artifact.json': validation_source.parent / 'release-artifact.json',
    'bundle-artifact.json': validation_source.parent / 'bundle-artifact.json',
}
for name, source in files.items(): shutil.copy2(source, directory / name)
guide = (directory / 'TESTING.md').read_text().replace('(trademark.md)', '(TRADEMARK.md)')
guide = re.sub(r'\[release evidence\]\([^)]+\)', '[release evidence](VALIDATION.md)', guide)
(directory / 'TESTING.md').write_text(guide)
# The handoff contains a readable summary, not device-specific test logs.
# Detailed relative report links remain valid in the source repository.
validation = (directory / 'VALIDATION.md').read_text()
validation = re.sub(r'\[([^\]]+)\]\((?!https?://)[^)]+\)', r'\1', validation)
(directory / 'VALIDATION.md').write_text(validation)
names = sorted([*files, 'NOTICES.tar.gz', 'SOURCE-MANIFEST.json', 'SOURCE-REVISION', source_archive.name])
lines = []
for name in names:
    with (directory / name).open('rb') as stream:
        lines.append(f'{hashlib.file_digest(stream, "sha256").hexdigest()}  {name}\n')
(directory / 'SHA256SUMS').write_text(''.join(lines))
print(directory)
