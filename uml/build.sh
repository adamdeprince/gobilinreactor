#!/usr/bin/env bash
# Run in a Debian 13 ARM64 Linux builder. The target NDK sysroot may come from
# any host package; the builder's clang/LLVM tools execute the compilation.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
test "$(uname -s)" = Linux || { echo 'Use a case-sensitive Linux build host for the kernel and initramfs.' >&2; exit 1; }
test "$(uname -m)" = aarch64 || { echo 'The guest initramfs builder currently requires an ARM64 Linux host.' >&2; exit 1; }
mkdir -p "$HERE/build"
python3 - "$HERE" <<'PY'
import json, pathlib, subprocess, sys
root=pathlib.Path(sys.argv[1]); lock=json.loads((root/'sources.lock.json').read_text())
for name in ('linux', 'passt'):
    pin=lock[name]; path=root/'build'/name
    if not path.exists():
        subprocess.run(['git','init',str(path)],check=True)
        subprocess.run(['git','-C',str(path),'fetch','--depth=1',pin['repository'],pin['commit']],check=True)
        subprocess.run(['git','-C',str(path),'checkout','--detach','FETCH_HEAD'],check=True)
    actual=subprocess.check_output(['git','-C',str(path),'rev-parse','HEAD'],text=True).strip()
    if actual!=pin['commit']: raise SystemExit(f'{path} has an unexpected revision; leave it intact and select the pinned source')
PY
bash "$HERE/build-kernel.sh"
bash "$HERE/build-passt.sh"
python3 "$HERE/build-initramfs.py"
python3 - "$HERE" <<'PY'
import hashlib,json,pathlib,subprocess,sys
root=pathlib.Path(sys.argv[1]); out=root/'build/artifacts'
manifest={'sources':json.loads((root/'sources.lock.json').read_text()),
          'compiler':subprocess.check_output(['clang','--version'],text=True).splitlines()[0],
          'inputs':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in [*root.iterdir(), *sorted((root/'patches').glob('*.patch'))] if p.is_file()},
          'artifacts':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.iterdir() if p.is_file() and p.name!='build-manifest.json'}}
(out/'build-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
PY
