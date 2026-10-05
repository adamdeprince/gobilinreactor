#!/usr/bin/env python3
"""Check release packaging, signing, page alignment and component inventory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__); p.add_argument('apk', type=Path); p.add_argument('--output', type=Path)
args = p.parse_args()
sdk = Path(os.environ.get('ANDROID_SDK_ROOT', Path.home() / 'Library/Android/sdk'))
bt = sdk / 'build-tools' / os.environ.get('BT_VER', '36.1.0')
env = dict(os.environ); env.setdefault('JAVA_HOME', '/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home')
def run(*command): return subprocess.check_output([str(c) for c in command], env=env, text=True, stderr=subprocess.STDOUT)
manifest = run(bt/'aapt2', 'dump', 'xmltree', args.apk, '--file', 'AndroidManifest.xml')
assert 'package="dev.goblinreactor.sentry"' in manifest
assert 'dev.goblinlinux' not in manifest
assert 'HarnessActivity' not in manifest and 'Acceptance' not in manifest and 'E: instrumentation' not in manifest
assert 'debuggable' in manifest and '0xffffffff' not in next(line for line in manifest.splitlines() if 'debuggable' in line)
assert '=true' not in next(line for line in manifest.splitlines() if 'debuggable' in line)
labels = [line for line in manifest.splitlines() if 'android:label' in line]
assert len(labels) == 2 and all('="GoblinReactor"' in line for line in labels), labels
version = json.loads((ROOT/'harness/version.json').read_text())
assert version['name'] in manifest
signature = run(bt/'apksigner', 'verify', '--verbose', '--print-certs', args.apk)
assert 'v3 scheme (APK Signature Scheme v3): true' in signature and 'CN=GoblinReactor' in signature
run(bt/'zipalign', '-c', '-P', '16', '4', args.apk)
with zipfile.ZipFile(args.apk) as apk:
    names = apk.namelist(); dex = apk.read('classes.dex')
    assert b'dev/goblinlinux' not in dex
    for name in names:
        if name.startswith('lib/') and name.endswith('.so'):
            assert b'Java_dev_goblinlinux_' not in apk.read(name), name
    assert b'HarnessActivity;' not in dex and not any(name + b';' in dex for name in
        (b'ServicesAcceptance', b'TerminalAcceptance', b'PowerAcceptance', b'ProductAcceptance'))
    assert b'Disable child process restrictions' not in dex
    assert 'assets/release-notices/LICENSE' in names and 'assets/release-notices/COPYING' in names
    assert any(name.startswith('assets/release-notices/debian/') for name in names)
    assert any(name.startswith('assets/release-notices/boot/') for name in names)
    assert 'assets/release-info.json' in names and any(name.startswith('assets/terminal-licenses/') for name in names)
    metadata = json.loads(apk.read('assets/release-info.json'))
    assert metadata['product_name'] == 'GoblinReactor'
    kernel_release = json.loads((ROOT/'uml/sources.lock.json').read_text())['linux']['version'] + '-goblin'
    assert metadata['kernel_release'] == kernel_release and '-rc' not in kernel_release
    assert ('Linux version ' + kernel_release + ' ').encode() in apk.read('lib/arm64-v8a/libgoblinuml-kernel.so')
    assert 'icon' in manifest
    assert 'assets/uml-acceptance.sh' not in names
    assert not any('keystore' in name or 'signing/' in name or 'password' == Path(name).name for name in names)
    native = {name:hashlib.sha256(apk.read(name)).hexdigest() for name in names if name.startswith('lib/') and name.endswith('.so')}
    for name in ['kernel', 'stub', 'netservice', 'ports']: assert 'lib/arm64-v8a/libgoblinuml-' + name + '.so' in native
    assert 'lib/arm64-v8a/libgoblinlauncher.so' in native
report = {'result':'PASS', 'version':version, 'kernel_release':kernel_release, 'apk_sha256':hashlib.sha256(args.apk.read_bytes()).hexdigest(), 'native_sha256':native,
          'checks':['non-debuggable', 'no test components/classes/assets', 'release RSA signing and v3 verification', '16 KiB ZIP alignment', 'no signing secrets']}
if args.output: args.output.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
