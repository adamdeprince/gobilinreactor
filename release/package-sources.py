#!/usr/bin/env python3
"""Package the committed project and verified corresponding-source archives."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import zipfile
import importlib.util

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
spec = importlib.util.spec_from_file_location('source_collector', HERE / 'collect-sources.py')
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)

def sha(path):
    with path.open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision', default='HEAD')
    args = parser.parse_args()
    revision = subprocess.check_output(['git', 'rev-parse', args.revision + '^{commit}'], cwd=ROOT, text=True).strip()
    version = json.loads((ROOT / 'harness/version.json').read_text())['name']
    output = ROOT / 'dist' / version; output.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(ROOT / 'harness/build-release/goblin-sentry.apk') as apk:
        release = json.loads(apk.read('assets/release-info.json'))
    if release['version']['name'] != version: raise SystemExit('Rebuild the release APK')
    project = HERE / 'build/project-source.tar'
    with project.open('wb') as stream:
        subprocess.run(['git', 'archive', '--format=tar', revision], cwd=ROOT, stdout=stream, check=True)
    inputs = dict(release['source_inputs'])
    manifest = {'version': version, 'source_revision': revision,
                'apk_sha256': sha(ROOT / 'harness/build-release/goblin-sentry.apk'), 'files': []}
    destination = output / f'GoblinReactor-{version}-corresponding-source.tar'
    temporary = destination.with_suffix('.tar.part')
    def add_bytes(archive, name, data, mode=0o644):
        member = tarfile.TarInfo(name); member.size = len(data); member.mode = mode
        archive.addfile(member, io.BytesIO(data))
        manifest['files'].append({'path': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    with tarfile.open(temporary, 'w') as archive:
        with tarfile.open(project) as source:
            for member in source:
                if member.isdir(): continue
                if not member.isfile(): raise ValueError('Unsupported project member: ' + member.name)
                data = source.extractfile(member).read()
                if member.name in inputs:
                    if hashlib.sha256(data).hexdigest() != inputs.pop(member.name):
                        raise SystemExit('APK source differs from commit: ' + member.name)
                add_bytes(archive, 'project/' + member.name, data, member.mode)
        if inputs: raise SystemExit('APK inputs missing from source commit: ' + ', '.join(inputs))
        items = list(collector.entries()) + json.loads((HERE / 'uml-archives.lock.json').read_text())
        for item in items:
            path = HERE / 'build/sources' / item['path']
            if sha(path) != item['sha256']: raise SystemExit('Source checksum mismatch: ' + item['path'])
            name = 'upstream/' + item['path']
            member = tarfile.TarInfo(name); member.size = path.stat().st_size; member.mode = 0o644
            with path.open('rb') as stream: archive.addfile(member, stream)
            manifest['files'].append({'path': name, 'size': member.size, 'sha256': item['sha256']})
        add_bytes(archive, 'SOURCE-REVISION', (revision + '\n').encode())
    temporary.replace(destination)
    manifest['archive'] = destination.name
    manifest['archive_sha256'] = sha(destination)
    (output / 'SOURCE-MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (output / 'SOURCE-REVISION').write_text(revision + '\n')
    print(destination)
    print(f'{len(manifest["files"])} files; {destination.stat().st_size // 2**20} MiB; source {revision}')

if __name__ == '__main__': main()
