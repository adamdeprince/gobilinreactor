#!/usr/bin/env python3
"""Restore terminal source caches from an unpacked corresponding-source handoff."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import shutil

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
spec = importlib.util.spec_from_file_location('collector', HERE / 'collect-sources.py')
collector = importlib.util.module_from_spec(spec); spec.loader.exec_module(collector)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--upstream', type=Path, default=ROOT.parent / 'upstream')
args = parser.parse_args()
cache = ROOT / 'terminal/build/cache/downloads'; cache.mkdir(parents=True, exist_ok=True)
for entry in collector.entries():
    source = args.upstream / entry['path']
    with source.open('rb') as stream: digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    if digest != entry['sha256']: raise SystemExit('Corrupt source: ' + str(source))
    if entry['path'].startswith(('terminal/', 'python-prebuilt/', 'license-inputs/')):
        shutil.copyfile(source, cache / entry['sha256'])
print('Source hashes verified; terminal cache restored')
