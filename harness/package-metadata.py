#!/usr/bin/env python3
"""Package readable notices and reproducible component identities for support."""
import hashlib
import json
from pathlib import Path
import sys
import shutil
import subprocess
import xml.etree.ElementTree as ET
import zipfile

root = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1])
assets = output / 'assets'
with zipfile.ZipFile(assets / 'kitty-runtime.zip') as archive:
    for name in archive.namelist():
        path = Path(name)
        if path.parts[0] != 'licenses' or name.endswith('/'):
            continue
        if '..' in path.parts or path.is_absolute():
            raise ValueError('Invalid notice path')
        destination = assets / 'terminal-licenses' / Path(*path.parts[1:])
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(archive.read(name))
# Preserve readable project, Debian, boot-tool and toolchain notices.
notices = assets / 'release-notices'
shutil.copytree(root / 'release/notices', notices, dirs_exist_ok=True)
for name in ('LICENSE', 'COPYING', 'THIRD_PARTY_NOTICES.md'):
    shutil.copyfile(root / name, notices / name)
boot = json.loads((root / 'release/boot-packages.lock.json').read_text())
assert boot['initramfs_manifest'] == json.loads((assets / 'uml-initramfs-manifest.json').read_text()), 'Refresh audited boot-package identities'
# Source hashes identify build inputs independently of later release-evidence commits.
paths = subprocess.check_output(['git', '-C', str(root), 'ls-files', '--cached', '--others', '--exclude-standard', '-z']).decode().split('\0')
source_inputs = {}
for name in sorted(set(paths)):
    path = root / name
    if (not name or not path.is_file() or name.startswith(('harness/results/', 'release/build/'))): continue
    if path.suffix in ('.md', '.txt', '.log'): continue
    if name.startswith(('harness/', 'uml/', 'terminal/', 'fixtures/', 'release/')) or name in ('LICENSE', 'COPYING'):
        source_inputs[name] = hashlib.sha256(path.read_bytes()).hexdigest()
uml_sources = json.loads((root / 'uml/sources.lock.json').read_text())
kernel_release = uml_sources['linux']['version'] + '-goblin'
kernel_bytes = (output / 'lib/arm64-v8a/libgoblinuml-kernel.so').read_bytes()
assert ('Linux version ' + kernel_release + ' ').encode() in kernel_bytes, 'Rebuild the kernel: binary release differs from stable source pin'
metadata = {
    'product_name': ET.parse(root / 'harness/AndroidManifest.xml').getroot().find('application').get('{http://schemas.android.com/apk/res/android}label'),
    'source_inputs': source_inputs,
    'version': json.loads((root / 'harness/version.json').read_text()),
    'kernel_release': kernel_release,
    'kernel_sha256': hashlib.sha256(kernel_bytes).hexdigest(),
    'initramfs_sha256': hashlib.sha256((assets / 'uml-initramfs.cpio.gz').read_bytes()).hexdigest(),
    'terminal_runtime': (assets / 'kitty-runtime-version.txt').read_text().strip(),
    'uml_sources': uml_sources,
    'kernel_patches': {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted((root / 'uml/patches').glob('*.patch'))},
    'fd_cleanup_sha256': hashlib.sha256((root / 'uml/fd-cleanup.h').read_bytes()).hexdigest(),
}
(assets / 'release-info.json').write_text(json.dumps(metadata, indent=2) + '\n')
