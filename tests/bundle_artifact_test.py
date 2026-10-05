#!/usr/bin/env python3
"""Check real bundle-generated splits, extraction and native ELF compatibility."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('apk', type=Path)
p.add_argument('bundle', type=Path)
p.add_argument('apks', type=Path)
p.add_argument('--output', type=Path)
args = p.parse_args()
sdk = Path(os.environ.get('ANDROID_SDK_ROOT', Path.home() / 'Library/Android/sdk'))
bt = sdk / 'build-tools' / os.environ.get('BT_VER', '36.1.0')
env = dict(os.environ)
env.setdefault('JAVA_HOME', '/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home')

def run(*command):
    return subprocess.check_output([str(c) for c in command], env=env, text=True, stderr=subprocess.STDOUT)

def identity(data): return hashlib.sha256(data).hexdigest()

with zipfile.ZipFile(args.apk) as apk:
    expected = {name: identity(apk.read(name)) for name in apk.namelist() if name.startswith(('lib/', 'assets/')) and not name.endswith('/')}
with zipfile.ZipFile(args.bundle) as bundle:
    actual = {name[5:]: identity(bundle.read(name)) for name in bundle.namelist() if name.startswith(('base/lib/', 'base/assets/')) and not name.endswith('/')}
    assert actual == expected, 'App Bundle changed or omitted packaged runtime/assets'
    assert 'base/manifest/AndroidManifest.xml' in bundle.namelist() and 'base/dex/classes.dex' in bundle.namelist()
    assert not any('keystore' in name or '/signing/' in name for name in bundle.namelist())

libraries, assets, splits = {}, {}, []
with zipfile.ZipFile(args.apks) as archive, tempfile.TemporaryDirectory(prefix='goblin-split-check-') as directory:
    for name in archive.namelist():
        if not name.startswith('splits/') or not name.endswith('.apk'): continue
        data = archive.read(name); path = Path(directory) / Path(name).name; path.write_bytes(data)
        signature = run(bt / 'apksigner', 'verify', '--verbose', '--print-certs', path)
        assert 'CN=GoblinReactor' in signature and 'v3 scheme (APK Signature Scheme v3): true' in signature
        run(bt / 'zipalign', '-c', '-P', '16', '4', path)
        manifest = run(bt / 'aapt2', 'dump', 'xmltree', path, '--file', 'AndroidManifest.xml')
        assert 'HarnessActivity' not in manifest and 'Acceptance' not in manifest
        if 'TerminalActivity' in manifest:
            extract = next(line for line in manifest.splitlines() if 'extractNativeLibs' in line)
            assert extract.rstrip().endswith('=true') or '0xffffffff' in extract, 'Native executable files must be extracted by Android'
            debug = next(line for line in manifest.splitlines() if 'debuggable' in line)
            assert debug.rstrip().endswith('=false') or ('0xffffffff' not in debug and '=true' not in debug)
        with zipfile.ZipFile(io.BytesIO(data)) as split:
            for entry in split.namelist():
                if entry.endswith('/'): continue
                if entry.startswith('assets/'): assets[entry] = identity(split.read(entry))
                if not entry.startswith('lib/') or not entry.endswith('.so'): continue
                elf = split.read(entry); libraries[entry] = identity(elf)
                assert elf[:6] == b'\x7fELF\x02\x01', entry
                assert struct.unpack_from('<H', elf, 18)[0] == 183, entry
                offset = struct.unpack_from('<Q', elf, 32)[0]
                size, count = struct.unpack_from('<HH', elf, 54)
                for i in range(count):
                    kind, flags, file_offset, virtual, physical, length, memory, align = struct.unpack_from('<IIQQQQQQ', elf, offset + i * size)
                    if kind == 1:
                        assert align >= 16384 and (virtual - file_offset) % 16384 == 0, entry + ' is not 16 KiB ELF aligned'
        splits.append(name)
assert splits and libraries
assert {**libraries, **assets} == expected, 'Device splits changed or omitted packaged runtime/assets'
report = {'result': 'PASS', 'aab_sha256': identity(args.bundle.read_bytes()), 'apks_sha256': identity(args.apks.read_bytes()),
          'splits': splits, 'native_components': len(libraries), 'assets': len(assets),
          'checks': ['release signatures', 'non-debuggable', 'native executable extraction retained',
                     '16 KiB ELF load alignment and APK ZIP alignment', 'runtime and asset hashes match release APK', 'no test components or signing keys']}
if args.output: args.output.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
