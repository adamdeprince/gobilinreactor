#!/usr/bin/env python3
"""Bundle pinned guest-only migrations; never execute Debian code on the host."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
from bootstrap import download

HERE = Path(__file__).resolve().parent


def build(output, offline=False):
    lock = (HERE / 'debian-deployment.lock.json').read_bytes()
    script = (HERE / 'deploy-guest.sh').read_bytes()
    packages = json.loads(lock)
    output.mkdir(parents=True, exist_ok=True)
    repository_lock = (HERE / 'goblinreactor-repository.lock.json').read_bytes()
    repository = json.loads(repository_lock)
    key = repository['key']
    key_bytes = (HERE / key['filename']).read_bytes()
    if hashlib.sha256(key_bytes).hexdigest() != key['sha256']:
        raise RuntimeError('SHA256 mismatch: bundled Goblin Reactor signing key')
    (output / key['filename']).write_bytes(key_bytes)
    sources = (
        'Types: deb deb-src\n'
        + 'URIs: ' + repository['uri'] + '\n'
        + 'Suites: ' + repository['suite'] + '\n'
        + 'Components: ' + ' '.join(repository['components']) + '\n'
        + 'Architectures: ' + ' '.join(repository['architectures']) + '\n'
        + 'Signed-By: /etc/apt/keyrings/' + key['filename'] + '\n')
    (output / 'goblinreactor.sources').write_text(sources)
    for package in packages:
        if offline and not (HERE / 'build' / Path(package['Filename']).name).is_file():
            raise RuntimeError('Run fixtures/deployment.py once to download ' + package['Package'])
        shutil.copyfile(download(package), output / Path(package['Filename']).name)
    (output / 'configure.sh').write_bytes(script)
    (output / 'manifest').write_text(''.join(p['Package'] + ' ' + p['Version'] + ' ' + Path(p['Filename']).name + '\n' for p in packages))
    (output / 'version').write_text(hashlib.sha256(lock + script + repository_lock + key_bytes + sources.encode()).hexdigest() + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, nargs='?', default=HERE / 'build/deployment')
    parser.add_argument('--offline', action='store_true')
    args = parser.parse_args()
    build(args.output, args.offline)
