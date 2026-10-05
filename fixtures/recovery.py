#!/usr/bin/env python3
"""Create deterministic, local-only dpkg upgrade fault fixtures."""
import io
import gzip
from pathlib import Path
import sys
import tarfile


def tar(files):
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode='w', format=tarfile.USTAR_FORMAT) as archive:
        directories = {parent for name, _, _ in files for parent in Path(name).parents if str(parent) != '.'}
        for directory in sorted(directories, key=lambda p: (len(p.parts), str(p))):
            info = tarfile.TarInfo('./' + str(directory) + '/')
            info.type, info.mode, info.mtime = tarfile.DIRTYPE, 0o755, 0
            archive.addfile(info)
        for name, data, mode in files:
            info = tarfile.TarInfo('./' + name)
            info.mode, info.size, info.mtime = mode, len(data), 0
            archive.addfile(info, io.BytesIO(data))
    return gzip.compress(out.getvalue(),mtime=0)


def package(version):
    control = f'Package: goblin-recovery-fixture\nVersion: {version}\nArchitecture: arm64\nMaintainer: Goblin acceptance <test@localhost>\nDescription: isolated upgrade recovery fixture\n'.encode()
    script = b'#!/bin/sh\nset -eu\nmkdir -p "$DPKG_ROOT/var/lib/goblin-proof"\n'
    if version == 2:
        script += b'printf checkpoint >"$DPKG_ROOT/var/lib/goblin-proof/checkpoint"\nsync\nwhile [ ! -f "$DPKG_ROOT/var/lib/goblin-proof/continue" ]; do sleep 1; done\n'
    script += b'cp "$DPKG_ROOT/usr/share/goblin-recovery/value" "$DPKG_ROOT/var/lib/goblin-proof/configured"\n'
    members = [('debian-binary', b'2.0\n'), ('control.tar.gz', tar([
        ('control', control, 0o644), ('postinst', script, 0o755),
        ('conffiles', b'/etc/goblin-recovery.conf\n', 0o644)])),
        ('data.tar.gz', tar([
            ('usr/share/goblin-recovery/value', f'version-{version}\n'.encode(), 0o644),
            ('usr/share/goblin-recovery/payload', b'G' * (2 << 20) if version == 2 else b'old\n', 0o644),
            ('etc/goblin-recovery.conf', f'seed-{version}\n'.encode(), 0o644)]))]
    out = bytearray(b'!<arch>\n')
    for name, data in members:
        out.extend(f'{name + "/":<16}{0:<12}{0:<6}{0:<6}{100644:<8}{len(data):<10}`\n'.encode())
        out.extend(data)
        if len(data) % 2: out.extend(b'\n')
    return out


if __name__ == '__main__':
    destination = Path(sys.argv[1]); destination.mkdir(parents=True, exist_ok=True)
    for version in (1, 2):
        (destination / f'recovery-{version}.deb').write_bytes(package(version))
