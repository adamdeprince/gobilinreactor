#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/build"
SDK="${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}"
BT="$SDK/build-tools/36.1.0"
NDK="$SDK/ndk/29.0.14206865/toolchains/llvm/prebuilt/darwin-x86_64"
JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}"
export JAVA_HOME PATH="$JAVA_HOME/bin:$PATH"
mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/lib/arm64-v8a"
CXX="$NDK/bin/aarch64-linux-android29-clang++"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wl,-z,max-page-size=16384 -static-libstdc++ -fPIC -shared "$HERE/launch.cpp" -o "$OUT/lib/arm64-v8a/libzygoteprobe.so"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wl,-z,max-page-size=16384 -static-libstdc++ "$HERE/child.cpp" -o "$OUT/lib/arm64-v8a/libzygotechild.so"
ANDROID_JAR="$SDK/platforms/android-36/android.jar"
javac --release 8 -classpath "$ANDROID_JAR" -d "$OUT/classes" "$HERE"/java/dev/goblinlinux/zygoteprobe/*.java
jar cf "$OUT/probe.jar" -C "$OUT/classes" .
"$BT/d8" --min-api 29 --lib "$ANDROID_JAR" --output "$OUT/dex" "$OUT/probe.jar"
"$BT/aapt2" link --manifest "$HERE/AndroidManifest.xml" -I "$ANDROID_JAR" --min-sdk-version 29 --target-sdk-version 36 -o "$OUT/unaligned.apk"
(cd "$OUT/dex" && zip -q "$OUT/unaligned.apk" classes.dex)
(cd "$OUT" && zip -q -r unaligned.apk lib)
"$BT/zipalign" -f -P 16 4 "$OUT/unaligned.apk" "$OUT/aligned.apk"
KEYSTORE="$HERE/../debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
    KEYSTORE="$OUT/research.keystore"
    if [ ! -f "$KEYSTORE" ]; then
        keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
            -alias goblindebug -keyalg RSA -keysize 2048 -validity 3650 \
            -dname "CN=Goblin research" >/dev/null
    fi
fi
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android --ks-key-alias goblindebug --out "$OUT/zygote-probe.apk" "$OUT/aligned.apk"
echo "Built $OUT/zygote-probe.apk"
