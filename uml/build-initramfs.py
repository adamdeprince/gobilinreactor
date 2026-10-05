#!/usr/bin/env python3
"""Build the UML boot tools in Linux and produce a deterministic initramfs."""
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess

HERE = Path(__file__).resolve().parent
OUT = HERE / 'build/artifacts'
OUT.mkdir(parents=True, exist_ok=True)
agent = OUT / 'goblin-guest'
subprocess.run(['g++', '-std=c++17', '-O2', '-static', '-pthread', '-Wall', '-Wextra',
                str(HERE / 'guest.cpp'), '-o', str(agent), '-lutil'], check=True)
entries = {'init': (stat.S_IFREG | 0o755, agent.read_bytes())}
packages = set()

def add(path):
    p = Path(path)
    name = str(p).lstrip('/')
    if name in entries:
        return
    entries[name] = (stat.S_IFREG | (p.stat().st_mode & 0o777), p.read_bytes())
    result = subprocess.run(['dpkg-query', '-S', str(p)], text=True, capture_output=True)
    for line in result.stdout.splitlines():
        if re.match(r'^[a-z0-9][a-z0-9+.-]*(?::[a-z0-9]+)?: ', line):
            packages.add(line.split(': ', 1)[0])

for path in ['/usr/sbin/mke2fs', '/usr/sbin/e2fsck', '/usr/sbin/resize2fs']:
    add(path)
    # Give the init process conventional /sbin paths regardless of usrmerge.
    entries['sbin/' + Path(path).name] = entries[str(path).lstrip('/')]
    libraries = subprocess.check_output(['ldd', path], text=True)
    for library in re.findall(r'(?:=>\s+)?(/\S+)\s+\(', libraries):
        add(library)
add('/etc/mke2fs.conf')
add('/bin/busybox')
for name in ['sh', 'mount', 'umount', 'ls', 'cat', 'cp', 'mv', 'rm', 'mkdir', 'sync', 'dmesg', 'df', 'du', 'ps', 'kill', 'less', 'vi', 'blkid', 'hexdump', 'reboot', 'poweroff']:
    entries['bin/' + name] = (stat.S_IFLNK | 0o777, b'busybox')
entries['etc/passwd'] = (stat.S_IFREG | 0o644, b'root:x:0:0:Rescue administrator:/root:/bin/sh\n')
entries['etc/group'] = (stat.S_IFREG | 0o644, b'root:x:0:\n')
entries['etc/profile'] = (stat.S_IFREG | 0o644, b'export PS1="rescue# "\n')
for name in list(entries):
    for parent in Path(name).parents:
        if str(parent) != '.':
            entries.setdefault(str(parent), (stat.S_IFDIR | 0o755, b''))
for name in ['dev', 'proc', 'sys', 'host', 'newroot', 'tmp', 'root', 'mnt', 'run']:
    entries.setdefault(name, (stat.S_IFDIR | (0o1777 if name == 'tmp' else 0o755), b''))
archive = bytearray()

def record(name, mode, data, inode):
    encoded = name.encode() + b'\0'
    fields = [inode, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(encoded), 0]
    archive.extend(b'070701' + ''.join(f'{v:08x}' for v in fields).encode())
    archive.extend(encoded)
    archive.extend(b'\0' * (-len(archive) % 4))
    archive.extend(data)
    archive.extend(b'\0' * (-len(archive) % 4))

for index, (name, (mode, data)) in enumerate(sorted(entries.items(), key=lambda e: (e[0].count('/'), e[0])), 1):
    record(name, mode, data, index)
record('TRAILER!!!', 0, b'', 0)
(OUT / 'initramfs.cpio.gz').write_bytes(gzip.compress(archive, compresslevel=9, mtime=0))
manifest = {
    'packages': subprocess.check_output(['dpkg-query', '-W', '-f=${Package} ${Version}\n', *sorted(packages)], text=True).splitlines(),
    'files': {name: hashlib.sha256(data).hexdigest() for name, (mode, data) in entries.items() if stat.S_ISREG(mode)},
}
(OUT / 'initramfs-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print('Built', OUT / 'initramfs.cpio.gz')
