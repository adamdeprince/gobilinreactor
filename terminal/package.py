#!/usr/bin/env python3
"""Package an Android CPython/kitty build for the offline harness builder."""
import argparse
import hashlib
import io
from pathlib import Path
import shutil
import subprocess
import zipfile


def write(z, name, data):
    entry = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
    entry.compress_type = z.compression
    entry.external_attr = 0o100644 << 16
    z.writestr(entry, data)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--python-prefix', type=Path, required=True)
    p.add_argument('--kitty-source', type=Path, required=True)
    p.add_argument('--native', type=Path, required=True)
    p.add_argument('--strip', type=Path, required=True)
    p.add_argument('--licenses', type=Path)
    p.add_argument('--output', type=Path, default=Path(__file__).parent / 'build')
    args = p.parse_args()
    assets, libraries = args.output / 'assets', args.output / 'lib'
    assets.mkdir(parents=True, exist_ok=True)
    libraries.mkdir(parents=True, exist_ok=True)
    for name in ('libpython3.13.so', 'libcrypto_python.so', 'libssl_python.so', 'libsqlite3_python.so'):
        shutil.copy2(args.python_prefix / 'lib' / name, libraries / name)
    shutil.copy2(args.native, libraries / 'libgoblinkitty.so')
    for library in libraries.glob('*.so'):
        subprocess.run([str(args.strip), '--strip-unneeded', str(library)], check=True)
    stdlib = io.BytesIO()
    with zipfile.ZipFile(stdlib, 'w', compression=zipfile.ZIP_STORED) as z:
        source = args.python_prefix / 'lib/python3.13'
        for path in sorted(source.rglob('*.py')):
            relative = path.relative_to(source)
            if any(x in relative.parts for x in ('test', 'tests', 'idlelib', 'tkinter', 'site-packages', '__pycache__')):
                continue
            write(z, str(relative), path.read_bytes())
    target = assets / 'kitty-runtime.zip'
    with zipfile.ZipFile(target, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        write(z, 'python313.zip', stdlib.getvalue())
        for path in sorted((source / 'lib-dynload').glob('*.so')):
            write(z, 'lib-dynload/' + path.name, path.read_bytes())
        for package in ('kitty', 'kittens'):
            for path in sorted((args.kitty_source / package).rglob('*.py')):
                write(z, 'app/' + str(path.relative_to(args.kitty_source)), path.read_bytes())
        write(z, 'app/engine.py', Path(__file__).with_name('engine.py').read_bytes())
        write(z, 'licenses/kitty-GPL-3.txt', (args.kitty_source / 'LICENSE').read_bytes())
        write(z, 'licenses/Python.txt', (args.python_prefix / 'lib/python3.13/LICENSE.txt').read_bytes())
        if args.licenses:
            for path in sorted(args.licenses.rglob('*')):
                if path.is_file():
                    write(z, 'licenses/dependencies/' + str(path.relative_to(args.licenses)), path.read_bytes())
        for name in ('sources.lock.json', 'python-deps.lock.json', 'licenses.lock.json'):
            write(z, name, Path(__file__).with_name(name).read_bytes())
    (assets / 'kitty-runtime-version.txt').write_text(hashlib.sha256(target.read_bytes()).hexdigest()[:16] + '\n')
    shutil.copy2(args.kitty_source / 'terminfo/kitty.terminfo', assets / 'kitty.terminfo')


if __name__ == '__main__':
    main()
