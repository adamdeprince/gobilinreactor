#!/usr/bin/env python3
"""Small, checked Android host-integration patches to the pinned UML source."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

root = Path(sys.argv[1])
header = root / 'arch/um/os-Linux/goblin-fds.h'
contents = (Path(__file__).parent / 'fd-cleanup.h').read_bytes()
if not header.exists() or header.read_bytes() != contents:
    header.write_bytes(contents)
p = root / 'arch/um/os-Linux/mem.c'
s = p.read_text()
before = '\tfd = make_tempfile(TEMPNAME_TEMPLATE);'
after = '''#ifdef __ANDROID__
	/* Guest RAM must use host memory, not consume the app's flash storage.
	 * memfd has no mount-size quota and Android can reclaim it as memory.
	 * Executable mappings are required for native guest instructions.
	 */
	if (!tempdir)
		tempdir = strdup(getenv("TMPDIR") ?: "/data/local/tmp");
	fd = memfd_create("goblin-uml-ram", MFD_CLOEXEC | MFD_ALLOW_SEALING | 0x0010U);
	if (fd < 0 && errno == EINVAL)
		fd = memfd_create("goblin-uml-ram", MFD_CLOEXEC | MFD_ALLOW_SEALING);
#else
	fd = make_tempfile(TEMPNAME_TEMPLATE);
#endif'''
if after not in s:
    if before not in s:
        raise RuntimeError('UML memory source changed')
    p.write_text(s.replace(before, after, 1))

# Later patches can edit earlier patches' lines. Validate the whole stack in
# staging, unwinding the existing prefix in reverse order before applying the
# current stack. Never partially patch the build tree on a mismatch.
patches = sorted((Path(__file__).parent / 'patches').glob('*.patch'))
paths = sorted({name for patch in patches for name in re.findall(r'^--- a/(.+)$', patch.read_text(), re.M)})
for name in paths:
    if Path(name).is_absolute() or '..' in Path(name).parts:
        raise RuntimeError('Unsafe kernel patch path')
failure = ''
for prefix in range(len(patches), -1, -1):
    with tempfile.TemporaryDirectory(prefix='goblin-kernel-patches-') as directory:
        staging = Path(directory)
        for name in paths:
            destination = staging / name; destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / name, destination)
        sequence = [(p, '--reverse') for p in reversed(patches[:prefix])] + [(p, '--forward') for p in patches]
        for patch, direction in sequence:
            result = subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-d', directory,
                                     '-i', str(patch.resolve()), direction], capture_output=True, text=True)
            if result.returncode or 'Unreversed patch detected' in result.stdout or 'Reversed (or previously applied)' in result.stdout:
                failure = f'{patch.name}: {result.stdout}{result.stderr}'
                break
        else:
            for name in paths:
                if (root / name).read_bytes() != (staging / name).read_bytes():
                    shutil.copy2(staging / name, root / name)
            break
else:
    raise RuntimeError('Kernel patch stack does not match the pinned source: ' + failure)
