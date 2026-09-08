#!/usr/bin/env bash
# Builds the goblin-linux capability probe APK without Gradle or AGP.
#
# Deliberate: the probe has no dependencies to resolve, no network access, and no
# AGP version compatibility surface. aapt2, clang, zipalign and apksigner are
# enough, and every step is visible.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/build"
SRC="$HERE/app/src/main"

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

# apksigner and keytool need a JVM; Homebrew's openjdk is keg-only so it is not
# on PATH by default.
if [ -z "${JAVA_HOME:-}" ]; then
    for candidate in \
        "$(brew --prefix openjdk@21 2>/dev/null)/libexec/openjdk.jdk/Contents/Home" \
        "$(/usr/libexec/java_home 2>/dev/null || true)"; do
        if [ -x "$candidate/bin/java" ]; then export JAVA_HOME="$candidate"; break; fi
    done
fi
[ -n "${JAVA_HOME:-}" ] || { echo "error: no JDK found; set JAVA_HOME" >&2; exit 1; }
export PATH="$JAVA_HOME/bin:$PATH"

for p in "$NDK" "$BT" "$ANDROID_JAR"; do
    [ -e "$p" ] || { echo "error: missing $p" >&2; exit 1; }
done

HOST_TAG="$(ls "$NDK/toolchains/llvm/prebuilt" | head -1)"
CXX="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin/aarch64-linux-android$MIN_API-clang++"
[ -x "$CXX" ] || { echo "error: no compiler at $CXX" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/lib/$ABI"

echo "==> compiling libgoblinprobe.so"
"$CXX" \
    -std=c++17 -O2 -g -fPIC -shared \
    -Wall -Wextra -Wno-unused-parameter \
    -fvisibility=hidden \
    -o "$OUT/lib/$ABI/libgoblinprobe.so" \
    "$SRC/cpp/probe.cpp" "$SRC/cpp/main.cpp" \
    -llog -landroid

echo "==> linking resources"
"$BT/aapt2" link \
    --manifest "$SRC/AndroidManifest.xml" \
    -I "$ANDROID_JAR" \
    --min-sdk-version "$MIN_API" \
    --target-sdk-version "$TARGET_API" \
    -o "$OUT/unaligned.apk"

echo "==> adding native library"
(cd "$OUT" && zip -q -r unaligned.apk "lib/$ABI/libgoblinprobe.so")

echo "==> aligning"
# -P 16 keeps .so files 16 KiB aligned, which Android 15+ devices require.
"$BT/zipalign" -f -P 16 4 "$OUT/unaligned.apk" "$OUT/aligned.apk"

KEYSTORE="$HERE/debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
    echo "==> generating debug keystore"
    keytool -genkeypair -v \
        -keystore "$KEYSTORE" -storepass android -keypass android \
        -alias goblindebug -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=goblin-linux debug, OU=probe, O=goblin-linux, C=US" >/dev/null
fi

echo "==> signing"
"$BT/apksigner" sign \
    --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
    --ks-key-alias goblindebug \
    --out "$OUT/goblin-probe.apk" \
    "$OUT/aligned.apk"

rm -f "$OUT/unaligned.apk" "$OUT/aligned.apk" "$OUT/goblin-probe.apk.idsig"
echo
echo "built: $OUT/goblin-probe.apk"
