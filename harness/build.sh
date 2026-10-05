#!/usr/bin/env bash
# Builds Goblin with the Linux/UML kernel and the existing Android terminal. No Gradle, no AGP, no network;
# The fixture builders supply the pinned Debian data beforehand.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
VARIANT="${VARIANT:-debug}"
case "$VARIANT" in debug) OUT="$HERE/build";; release) OUT="$HERE/build-release";; *) echo 'VARIANT must be debug or release' >&2; exit 1;; esac

SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Library/Android/sdk}}"
NDK_VER="${NDK_VER:-29.0.14206865}"
BT_VER="${BT_VER:-36.1.0}"
COMPILE_API="${COMPILE_API:-36}"
TARGET_API="${TARGET_API:-36}"
MIN_API="${MIN_API:-29}"
ABI="arm64-v8a"

NDK="$SDK/ndk/$NDK_VER"
BT="$SDK/build-tools/$BT_VER"
ANDROID_JAR="$SDK/platforms/android-$COMPILE_API/android.jar"

if [ -z "${JAVA_HOME:-}" ]; then
    for candidate in \
        "$(brew --prefix openjdk@21 2>/dev/null)/libexec/openjdk.jdk/Contents/Home" \
        "$(/usr/libexec/java_home 2>/dev/null || true)"; do
        if [ -x "$candidate/bin/java" ]; then export JAVA_HOME="$candidate"; break; fi
    done
fi
[ -n "${JAVA_HOME:-}" ] || { echo "error: no JDK found; set JAVA_HOME" >&2; exit 1; }
export PATH="$JAVA_HOME/bin:$PATH"

HOST_TAG="$(ls "$NDK/toolchains/llvm/prebuilt" | head -1)"
TOOLS="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin"
CC="$TOOLS/aarch64-linux-android$MIN_API-clang"
CXX="$TOOLS/aarch64-linux-android$MIN_API-clang++"

rm -rf "$OUT"
mkdir -p "$OUT/lib/$ABI" "$OUT/assets"
TERMINAL="$ROOT/terminal/build"
if [ ! -f "$TERMINAL/lib/libgoblinkitty.so" ]; then
    echo "error: build the native kitty runtime with terminal/build.py first" >&2
    exit 1
fi
cp "$TERMINAL"/lib/*.so "$OUT/lib/$ABI/"
cp "$TERMINAL"/assets/* "$OUT/assets/"
python3 "$ROOT/fixtures/deployment.py" --offline "$OUT/assets/deployment"
UML="$ROOT/uml/build/artifacts"
for file in libgoblinuml-kernel.so libgoblinuml-stub.so libgoblinuml-netservice.so libgoblinuml-ports.so; do
    test -f "$UML/$file" || { echo "Missing $UML/$file: build the UML artifacts first" >&2; exit 1; }
    cp "$UML/$file" "$OUT/lib/$ABI/$file"
done
cp "$UML/initramfs.cpio.gz" "$OUT/assets/uml-initramfs.cpio.gz"
if [ "$VARIANT" = debug ]; then cp "$ROOT/uml/acceptance.sh" "$OUT/assets/uml-acceptance.sh"; fi
cp "$ROOT/fixtures/build/debian.pack" "$OUT/assets/debian.pack"
cp "$ROOT/uml/sources.lock.json" "$OUT/assets/uml-sources.json"
cp "$UML/initramfs-manifest.json" "$OUT/assets/uml-initramfs-manifest.json"
cp -R "$UML/licenses" "$OUT/assets/uml-licenses"
python3 "$HERE/package-metadata.py" "$OUT"

echo "==> building UML terminal and lifecycle adapter"
"$CXX" -std=c++17 -O2 -g -fPIC -shared -Wall -Wextra -fvisibility=hidden \
    -static-libstdc++ -Wl,-z,max-page-size=16384 \
    "$ROOT/uml/launcher.cpp" -o "$OUT/lib/$ABI/libgoblinlauncher.so"
"$CXX" -std=c++17 -O2 -g -fPIC -shared -Wall -Wextra -fvisibility=hidden \
    -static-libstdc++ -Wl,-z,max-page-size=16384 \
    "$ROOT/uml/runtime.cpp" "$ROOT/uml/archive.cpp" "$ROOT/uml/maintenance.cpp" \
    -L "$OUT/lib/$ABI" -lgoblinkitty -llog -landroid -lz \
    -o "$OUT/lib/$ABI/libgoblinuml.so"
"$CXX" -std=c++17 -O2 -fPIE -pie -static-libstdc++ -Wl,-z,max-page-size=16384 \
    "$ROOT/uml/control.cpp" -o "$OUT/lib/$ABI/libgoblinuml-ctl.so"

echo "==> compiling terminal application"
mkdir -p "$OUT/classes" "$OUT/dex"
SOURCES=()
for source in "$HERE"/java/dev/goblinlinux/sentry/*.java; do
    case "$source" in */ServicesAcceptance.java) continue;; esac
    if [ "$VARIANT" = release ]; then
        case "$source" in *Acceptance.java|*/HarnessActivity.java) continue;; esac
    fi
    SOURCES+=("$source")
done
"$JAVA_HOME/bin/javac" --release 8 -Xlint:-options -classpath "$ANDROID_JAR" \
    -d "$OUT/classes" "${SOURCES[@]}"
"$JAVA_HOME/bin/jar" cf "$OUT/terminal.jar" -C "$OUT/classes" .
"$BT/d8" --min-api "$MIN_API" --lib "$ANDROID_JAR" --output "$OUT/dex" "$OUT/terminal.jar"

echo "==> linking resources"
python3 "$HERE/manifest.py" "$VARIANT" "$OUT/AndroidManifest.xml"
"$BT/aapt2" compile --dir "$HERE/res" -o "$OUT/resources.zip"
"$BT/aapt2" link \
    --manifest "$OUT/AndroidManifest.xml" \
    -I "$ANDROID_JAR" \
    -A "$OUT/assets" \
    "$OUT/resources.zip" \
    --min-sdk-version "$MIN_API" \
    --target-sdk-version "$TARGET_API" \
    -o "$OUT/unaligned.apk"

echo "==> adding native library"
(cd "$OUT" && zip -q -r unaligned.apk "lib/$ABI")
(cd "$OUT/dex" && zip -q "$OUT/unaligned.apk" classes.dex)

echo "==> aligning"
"$BT/zipalign" -f -P 16 4 "$OUT/unaligned.apk" "$OUT/aligned.apk"

echo "==> signing"
python3 "$HERE/sign.py" "${SIGNING:-$VARIANT}" "$OUT/aligned.apk" "$OUT/goblin-sentry.apk" --build-tools "$BT"

rm -f "$OUT/unaligned.apk" "$OUT/aligned.apk" "$OUT/goblin-sentry.apk.idsig"
echo
echo "built: $OUT/goblin-sentry.apk"
