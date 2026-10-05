#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE="${PASST_SOURCE:-$HERE/build/passt}"
NDK_TOOLS="${NDK_TOOLS:?Set NDK_TOOLS to the Android NDK prebuilt directory}"
RESOURCE="$(find "$NDK_TOOLS/lib/clang" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -1)"
python3 "$HERE/patch-passt.py" "$SOURCE"
mkdir -p "$HERE/build/artifacts"
if [ "${PASST_MANAGED_ONLY:-0}" != 1 ]; then
make -C "$SOURCE" -B passt pesto \
    CC="${CLANG:-clang} --target=aarch64-linux-android30 --sysroot=$NDK_TOOLS/sysroot -resource-dir=$RESOURCE --rtlib=compiler-rt --unwindlib=libunwind -fuse-ld=lld -Wno-unused-command-line-argument" \
    VERSION=goblin CPPFLAGS="-include $HERE/passt-android.h" \
    LDFLAGS="-Wl,-z,max-page-size=16384"
mkdir -p "$HERE/build/artifacts"
cp "$SOURCE/passt" "$HERE/build/artifacts/libgoblinuml-net.so"
cp "$SOURCE/pesto" "$HERE/build/artifacts/libgoblinuml-ports.so"
fi
# Reuse the pinned source list and generated headers for the Android service.
# Keep the standalone executables above available for host diagnostics.
sources="$(python3 - "$SOURCE/Makefile" <<'PY'
import pathlib, re, sys
text=pathlib.Path(sys.argv[1]).read_text().replace('\\\n', ' ')
print(re.search(r'^PASST_SRCS = (.*)$', text, re.M).group(1))
PY
)"
make -C "$SOURCE" -B passt \
    CC="${CLANG:-clang} --target=aarch64-linux-android30 --sysroot=$NDK_TOOLS/sysroot -resource-dir=$RESOURCE --rtlib=compiler-rt --unwindlib=libunwind -fuse-ld=lld -Wno-unused-command-line-argument" \
    VERSION=goblin CPPFLAGS="-include $HERE/passt-android.h -DGOBLIN_MANAGED_NETWORK -Dmain=goblin_passt_main" \
    CFLAGS="-fPIC -Wframe-larger-than=131072" LDFLAGS="-shared -Wl,-z,max-page-size=16384" \
    PASST_SRCS="$sources $HERE/network-service.c"
cp "$SOURCE/passt" "$HERE/build/artifacts/libgoblinuml-netservice.so"
mkdir -p "$HERE/build/artifacts/licenses"
cp "$SOURCE/LICENSES/GPL-2.0-or-later.txt" "$HERE/build/artifacts/licenses/passt-GPL-2.0-or-later.txt"
cp "$SOURCE/LICENSES/BSD-3-Clause.txt" "$HERE/build/artifacts/licenses/passt-BSD-3-Clause.txt"
