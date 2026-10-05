#!/usr/bin/env python3
"""Collect checksum-verified upstream archives without unpacking/executing them."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

def sha(path):
    with path.open('rb') as stream: return hashlib.file_digest(stream, 'sha256').hexdigest()

def entries():
    for source in json.loads((HERE / 'debian-sources.lock.json').read_text())['sources']:
        yield from source['files']
    for name, source in json.loads((ROOT / 'terminal/sources.lock.json').read_text()).items():
        suffix = '.whl' if source['url'].endswith('.whl') else '.tar.xz' if source['url'].endswith('.xz') else '.tar.gz'
        yield {**source, 'path': f'terminal/{name}{suffix}'}
    for source in json.loads((ROOT / 'terminal/python-deps.lock.json').read_text()):
        yield {**source, 'path': 'python-prebuilt/' + source['filename']}
    for source in json.loads((ROOT / 'terminal/licenses.lock.json').read_text()):
        yield {**source, 'path': 'license-inputs/' + source['name'] + ('.tar.gz' if 'member' in source else '.txt')}
    extra = HERE / 'extra-sources.lock.json'
    if extra.exists(): yield from json.loads(extra.read_text())

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--offline', action='store_true')
    args = parser.parse_args()
    output = HERE / 'build/sources'; output.mkdir(parents=True, exist_ok=True)
    cache = ROOT / 'terminal/build/cache/downloads'
    items = list(entries())
    assert len({item['path'] for item in items}) == len(items)
    def collect(item):
        destination = output / item['path']
        assert destination.resolve().is_relative_to(output.resolve())
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists():
            cached = cache / item['sha256']
            if cached.exists(): shutil.copyfile(cached, destination)
            elif args.offline: raise RuntimeError('Missing source: ' + item['path'])
            else:
                temporary = destination.with_suffix(destination.suffix + '.part')
                subprocess.run(['curl', '-fLsS', '--retry', '2', '--max-time', '300', '--proto', '=https',
                                '--proto-redir', '=https', item['url'], '-o', temporary], check=True)
                if sha(temporary) != item['sha256']: raise RuntimeError('Source checksum mismatch: ' + item['path'])
                temporary.replace(destination)
        if sha(destination) != item['sha256']: raise RuntimeError('Source checksum mismatch: ' + item['path'])
        print('Verified ' + item['path'], flush=True)
    with ThreadPoolExecutor(max_workers=4) as pool: list(pool.map(collect, items))
    (output / 'upstream-manifest.json').write_text(json.dumps(items, indent=2) + '\n')
    print(f'Collected {len(items)} verified files')

if __name__ == '__main__': main()
