#!/usr/bin/env python3
"""On the original Linux builder, identify each unmodified boot-image payload."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

manifest = json.load(sys.stdin)
packages = {}
files = {}
for name, expected in manifest['files'].items():
    if name in ('init', 'etc/passwd', 'etc/group', 'etc/profile'):
        continue
    path = Path('/') / name
    assert hashlib.sha256(path.read_bytes()).hexdigest() == expected, f'Builder differs: {name}'
    candidates = [str(path), str(path.resolve())]
    if str(path).startswith(('/bin/', '/sbin/', '/lib/')):
        candidates.append('/usr' + str(path))
    owners = []
    for candidate in candidates:
        result = subprocess.run(['dpkg-query', '-S', candidate], capture_output=True, text=True)
        owners += [line.split(': ', 1)[0] for line in result.stdout.splitlines() if ': ' in line and not line.startswith('diversion')]
    assert owners, f'Unowned payload: {name}'
    owner = sorted(set(owners))[0]
    files[name] = {'sha256': expected, 'binary_package': owner}
    packages[owner] = None
# The custom /init links the Debian C/C++ runtimes statically.
for name in ('libc6-dev', 'libgcc-14-dev', 'libstdc++-14-dev'):
    packages[name] = None
for name in packages:
    fields = subprocess.check_output(['dpkg-query', '-W', '-f=${Package}\t${Version}\t${source:Package}\t${source:Version}', name], text=True).split('\t')
    packages[name] = dict(zip(('package', 'version', 'source', 'source_version'), fields))
print(json.dumps({'files': files, 'packages': list(packages.values()), 'initramfs_manifest': manifest}, indent=2))
