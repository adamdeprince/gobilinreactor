#!/usr/bin/env python3
"""Round-trip the legacy inode/xattr representation through the UML importer.

Run as root in the isolated Linux builder (not the development host).
"""
import os
from pathlib import Path
import stat
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
assert os.uname().sysname == 'Linux' and os.geteuid() == 0, 'Use the Linux build VM'
with tempfile.TemporaryDirectory(prefix='goblin-migration-') as temporary:
    t = Path(temporary)
    driver = t / 'export.cpp'
    driver.write_text('''#include "archive.h"
#include <cstdio>
int main(int n, char** v) { std::string e; if (n != 3) return 2;
if (!goblin_uml::ExportLegacy(v[1],v[2],&e)) {fprintf(stderr,"%s\\n",e.c_str());return 1;} }
''')
    exporter = t / 'export'
    subprocess.run(['g++', '-std=c++17', '-O2', '-I'+str(ROOT/'uml'), str(driver),
                    str(ROOT/'uml/archive.cpp'), '-o', str(exporter)], check=True)
    src, dst = t/'old', t/'new'
    src.mkdir(); dst.mkdir()
    (src/'.goblin-inodes').mkdir(); (src/'home').mkdir()
    def metadata(path, mode, uid, gid):
        os.setxattr(path, 'user.goblin.metadata.v1', struct.pack('<IIII', 0x474d4431, mode, uid, gid))
    original = src/'.goblin-inodes/100-200'
    original.write_bytes(b'preserve package and user data\n')
    metadata(original, 0o640, 1000, 1001)
    os.utime(original, ns=(1700000000123456789,)*2)
    (src/'home/a').symlink_to('goblin-inode:100-200')
    (src/'home/b').symlink_to('goblin-inode:100-200')
    link = src/'home/symlink'; link.touch()
    metadata(link, 0o777, 1000, 1001)
    os.setxattr(link, 'user.goblin.symlink.v1', b'a')
    special = src/'home/socket'; special.touch()
    metadata(special, 0o600, 1000, 1001)
    os.setxattr(special, 'user.goblin.socket.v1', struct.pack('<I', 0x47534f31))
    metadata(src/'home', 0o750, 1000, 1001)
    # An interrupted promotion must recover the missing name without writing
    # to the original tree, where the old app's recovery journal lives.
    (src/'.goblin-inodes/promotion').write_bytes(b'100-200\0/home/recovered\0')
    (src/'usr').mkdir(); (src/'usr/bin').mkdir(); (src/'bin').symlink_to('usr/bin')
    (src/'usr/bin/tool').write_bytes(b'package executable\n')
    metadata(src/'usr/bin/tool', 0o4755, 0, 0)
    archive = t/'migration.pack'
    subprocess.run([str(exporter), str(src), str(archive)], check=True)
    assert not (src/'home/recovered').exists()
    assert original.read_bytes() == b'preserve package and user data\n'
    subprocess.run([str(ROOT/'uml/build/artifacts/goblin-guest'), '--unpack', str(archive), str(dst)], check=True)
    a, b, recovered = [(dst/'home'/n).stat() for n in ('a','b','recovered')]
    assert a.st_ino == b.st_ino == recovered.st_ino and a.st_nlink == 3
    assert (a.st_uid, a.st_gid, stat.S_IMODE(a.st_mode), a.st_mtime_ns) == (1000,1001,0o640,1700000000123456789)
    assert os.readlink(dst/'home/symlink') == 'a' and (dst/'home/symlink').lstat().st_uid == 1000
    assert stat.S_ISSOCK((dst/'home/socket').stat().st_mode)
    assert stat.S_IMODE((dst/'home').stat().st_mode) == 0o750
    assert stat.S_IMODE((dst/'bin/tool').stat().st_mode) == 0o4755
    assert not (dst/'.goblin-inodes').exists()
    os.setxattr(original, 'user.goblin.metadata.v1', b'corrupt')
    assert subprocess.run([str(exporter), str(src), str(t/'invalid.pack')]).returncode != 0
    assert not (t/'invalid.pack').exists()
print('PASS: migration ownership, permissions, hard links, symlinks, sockets, journal recovery, usrmerge and invalid metadata')
