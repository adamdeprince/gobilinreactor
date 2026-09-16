#!/usr/bin/env bash
# Builds the phase 1 harness APK: the guest program, the sentry, and the
# NativeActivity that runs one under the other. No Gradle, no AGP, no network.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="$HERE/build"

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

# The guest: position-independent so the loader can bias it into the guest
# window, and with no PT_INTERP, because a guest dynamic linker is phase 2.
echo "==> building guest"
for g in hello bench; do
    "$CC" \
        -O2 -fPIE -pie -nostdlib -nostartfiles \
        -Wl,--no-dynamic-linker -Wl,-e,_start \
        -o "$OUT/assets/$g" \
        "$ROOT/guest/$g.c"
    echo "    $g: $("$TOOLS/llvm-readelf" -h "$OUT/assets/$g" | awk -F: '/Type:/{gsub(/^ +/,"",$2); printf "%s", $2}')"
done

echo "==> building sentry + harness"
"$CXX" \
    -std=c++17 -O2 -g -fPIC -shared \
    -Wall -Wextra -Wno-unused-parameter \
    -fvisibility=hidden \
    -static-libstdc++ \
    -I "$ROOT/sentry" \
    -o "$OUT/lib/$ABI/libgoblinsentry.so" \
    "$ROOT/sentry/guest_layout.cpp" \
    "$ROOT/sentry/exec_memory.cpp" \
    "$ROOT/sentry/elf_loader.cpp" \
    "$ROOT/sentry/sentry.cpp" \
    "$ROOT/sentry/stub.cpp" \
    "$HERE/main.cpp" \
    -llog -landroid

echo "==> linking resources"
"$BT/aapt2" link \
    --manifest "$HERE/AndroidManifest.xml" \
    -I "$ANDROID_JAR" \
    -A "$OUT/assets" \
    --min-sdk-version "$MIN_API" \
    --target-sdk-version "$TARGET_API" \
    -o "$OUT/unaligned.apk"

echo "==> adding native library"
(cd "$OUT" && zip -q -r unaligned.apk "lib/$ABI/libgoblinsentry.so")

echo "==> aligning"
"$BT/zipalign" -f -P 16 4 "$OUT/unaligned.apk" "$OUT/aligned.apk"

KEYSTORE="$ROOT/probe/debug.keystore"
[ -f "$KEYSTORE" ] || {
    keytool -genkeypair -v -keystore "$KEYSTORE" -storepass android \
        -keypass android -alias goblindebug -keyalg RSA -keysize 2048 \
        -validity 10000 \
        -dname "CN=goblin-linux debug, OU=probe, O=goblin-linux, C=US" >/dev/null
}

echo "==> signing"
"$BT/apksigner" sign \
    --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
    --ks-key-alias goblindebug \
    --out "$OUT/goblin-sentry.apk" \
    "$OUT/aligned.apk"

rm -f "$OUT/unaligned.apk" "$OUT/aligned.apk" "$OUT/goblin-sentry.apk.idsig"
echo
echo "built: $OUT/goblin-sentry.apk"
