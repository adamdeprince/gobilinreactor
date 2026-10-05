#!/usr/bin/env python3
"""Exercise the production backup codec and atomic restore on Linux ext4."""
import fcntl
import gzip
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='goblin-disk-test-') as name:
    temp = Path(name)
    tool = temp / 'disk-tool'
    subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', str(ROOT/'tests/disk_backup_tool.cpp'), '-lz', '-o', str(tool)], check=True)
    directory = temp / 'uml'; directory.mkdir()
    disk = directory / 'rootfs.ext4'
    with disk.open('wb') as f: f.truncate(64*1024*1024)
    subprocess.run(['mke2fs', '-q', '-t', 'ext4', '-F', str(disk)], check=True)
    # Sparse address range exceeds the old disk quota; no large allocation.
    size = 100*1024**3
    with disk.open('r+b') as f:
        f.truncate(size); f.seek(size-32); f.write(b'last allocated page survives!!!\n')
    archive = temp / 'disk.goblin.gz'
    def invoke(action, backup=archive, ok=True):
        result = subprocess.run([str(tool), action, str(directory), str(backup)], capture_output=True, text=True)
        assert (result.returncode == 0) == ok, result.stdout + result.stderr
        return result
    def state():
        with disk.open('rb') as f:
            front=hashlib.sha256(f.read(64*1024**2)).hexdigest(); f.seek(size-32); tail=f.read()
        return disk.stat().st_ino, disk.stat().st_size, front, tail
    before = state()
    with (temp/'debian.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        invoke('export', ok=False)
    invoke('export')
    assert archive.stat().st_size < 8*1024**2
    assert before == state()
    print('PASS: live-disk lock enforced; sparse 100 GiB image exported without allocating holes')
    incomplete = directory / 'rootfs.restore.a1B2c3'
    incomplete.write_bytes(b'interrupted restore')
    unrelated = directory / 'rootfs.restore.notes'; unrelated.write_text('retain')
    symbolic = directory / 'rootfs.restore.d4E5f6'; symbolic.symlink_to(unrelated.name)
    previous = Path(invoke('import').stdout.strip())
    assert not incomplete.exists() and unrelated.read_text() == 'retain' and symbolic.is_symlink()
    symbolic.unlink(); unrelated.unlink()
    print('PASS: retry reclaims abandoned private restore data without following links or deleting unrelated files')
    after = state()
    assert after[0] != before[0] and after[1:] == before[1:]
    assert previous.stat().st_ino == before[0]
    assert disk.stat().st_blocks * 512 < 128*1024**2
    print('PASS: atomic restore preserves every sampled byte, sparse size and the previous disk')
    damaged = temp/'damaged.gz'; original = archive.read_bytes()
    for data in (original[:-5], original[:len(original)//2], original[:-8]+b'\0'*8):
        damaged.write_bytes(data); invoke('import', damaged, ok=False); assert state() == after
    invalid = struct.pack('<8sQ', b'GBLNDK01', size) + struct.pack('<QQ', size-1, 16) + b'x'*16
    damaged.write_bytes(gzip.compress(invalid)); invoke('import', damaged, ok=False); assert state() == after
    assert not list(directory.glob('rootfs.restore.*'))
    print('PASS: truncated, checksum-damaged and out-of-bounds backups leave the active disk intact')
    result = invoke('export', Path('/dev/full'), ok=False)
    assert state() == after
    print('PASS: destination out-of-space failure preserves the running installation backup source')
    retained = Path(invoke('previous', previous.name).stdout.strip())
    recovered = state()
    assert recovered[0] == before[0] and recovered[1:] == before[1:]
    assert retained == previous and retained.stat().st_ino == after[0]
    with disk.open('r+b') as f: f.seek(size-32); f.write(b'changed')
    with previous.open('rb') as f: f.seek(size-32); assert f.read() == before[3]
    invoke('previous', '../rootfs.ext4', ok=False)
    print('PASS: restoring a previous disk atomically swaps independent images without needing another full copy')
print('GOBLIN DISK BACKUP PASS')
