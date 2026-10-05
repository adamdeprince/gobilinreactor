#!/usr/bin/env python3
"""Check clean/idempotent UML patch application on a Linux build host."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pinned_source', type=Path, help='Unmodified pinned kernel source')
    parser.add_argument('--compiled-source', type=Path)
    parser.add_argument('--port-patch', type=Path, help='Also compare every added, changed or deleted ARM64 port file')
    args = parser.parse_args()
    uml = Path(__file__).resolve().parents[1] / 'uml'
    files = {'arch/um/os-Linux/mem.c', 'arch/um/os-Linux/goblin-fds.h'}
    if args.port_patch:
        for line in args.port_patch.read_text().splitlines():
            if line.startswith('diff --git '):
                files.update(path[2:] for path in line.split()[2:])
    patches = sorted((uml / 'patches').glob('*.patch'))
    for patch in patches:
        files.update(line[6:] for line in patch.read_text().splitlines() if line.startswith('+++ b/'))
    with tempfile.TemporaryDirectory(prefix='goblin-kernel-patch-') as directory:
        root = Path(directory)
        for name in files:
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            if (args.pinned_source / name).is_file(): shutil.copyfile(args.pinned_source / name, target)
        for attempt in range(2):
            subprocess.run(['python3', str(uml / 'patch-kernel.py'), str(root)], check=True)
            state = {name: (root / name).read_bytes() if (root / name).is_file() else None for name in files}
            if attempt == 0:
                applied = state
            else:
                assert applied == state, 'second application changed sources'
        if args.compiled_source:
            for name, content in applied.items():
                compiled = args.compiled_source / name
                assert content == (compiled.read_bytes() if compiled.is_file() else None), name
        print(f'PASS: {len(files)} patched files, clean application and idempotent repeat')
        if args.compiled_source:
            print('PASS: patched source matches the compiled tree')
        for patch in patches:
            print(patch.name, hashlib.sha256(patch.read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
