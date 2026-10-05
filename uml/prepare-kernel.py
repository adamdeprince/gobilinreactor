#!/usr/bin/env python3
"""Verify stable Linux source and apply the pinned ARM64 UML port separately."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile

HERE = Path(__file__).resolve().parent

def sha(path):
    with path.open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()

def version(path):
    values = dict(re.findall(r'^(VERSION|PATCHLEVEL|SUBLEVEL|EXTRAVERSION)[ \t]*=[ \t]*(.*?)[ \t]*$', (path / 'Makefile').read_text(), re.M))
    return '.'.join(values[key] for key in ('VERSION', 'PATCHLEVEL', 'SUBLEVEL')) + values['EXTRAVERSION']

def main():
    pin = json.loads((HERE / 'sources.lock.json').read_text())['linux']
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, default=HERE / 'build/downloads' / Path(pin['url']).name)
    parser.add_argument('--source', type=Path, default=HERE / 'build' / pin['directory'])
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--path-only', action='store_true')
    args = parser.parse_args()
    if args.path_only:
        print(args.source); return
    assert re.fullmatch(r'\d+\.\d+\.\d+', pin['version']), 'Only stable releases are allowed'
    patch = HERE / pin['arm64_port']['patch']
    assert sha(patch) == pin['arm64_port']['sha256'], 'ARM64 UML port checksum mismatch'
    stamp = args.source / '.goblin-kernel-source.json'
    if args.source.exists():
        if not stamp.exists() or json.loads(stamp.read_text()) != pin:
            raise SystemExit('Existing source does not match the pin; select a new --source directory')
        assert version(args.source) == pin['version'], 'Kernel version differs from the stable pin'
        print(args.source); return
    args.archive.parent.mkdir(parents=True, exist_ok=True)
    if not args.archive.exists():
        if args.offline: raise SystemExit('Missing stable kernel archive: ' + str(args.archive))
        temporary = args.archive.with_suffix('.download')
        subprocess.run(['curl', '-fLsS', '--retry', '2', '--max-time', '300', '--proto', '=https',
                        '--proto-redir', '=https', pin['url'], '-o', temporary], check=True)
        if sha(temporary) != pin['sha256']: raise SystemExit('Stable kernel archive checksum mismatch')
        temporary.replace(args.archive)
    assert sha(args.archive) == pin['sha256'], 'Stable kernel archive checksum mismatch'
    args.source.parent.mkdir(parents=True, exist_ok=True)
    # Extract Linux only onto a case-sensitive filesystem.
    with tempfile.TemporaryDirectory(prefix='goblin-kernel-', dir=args.source.parent) as directory:
        case_probe = Path(directory) / 'case'; case_probe.write_text('probe')
        if case_probe.with_name('CASE').exists(): raise SystemExit('Use a case-sensitive Linux build filesystem')
        with tarfile.open(args.archive) as archive:
            assert all(Path(member.name).parts[0] == pin['directory'] for member in archive)
            archive.extractall(directory, filter='data')
        source = Path(directory) / pin['directory']
        assert version(source) == pin['version'], 'Archive is not the pinned stable kernel'
        result = subprocess.run(['patch', '--batch', '--forward', '--fuzz=0', '-p1', '-i', str(patch)],
                                cwd=source, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode: raise SystemExit(result.stdout)
        assert version(source) == pin['version'], 'Port must not change the upstream release version'
        (source / stamp.name).write_text(json.dumps(pin, indent=2) + '\n')
        source.rename(args.source)
    print(args.source)

if __name__ == '__main__': main()
