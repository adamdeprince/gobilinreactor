#!/usr/bin/env bash
# Test-only APK, signed like its target. It is never part of the release APK.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Library/Android/sdk}}"
BT="$SDK/build-tools/${BT_VER:-36.1.0}"
ANDROID_JAR="$SDK/platforms/android-${COMPILE_API:-36}/android.jar"
JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}"
export JAVA_HOME PATH="$JAVA_HOME/bin:$PATH"
TARGET="${TARGET_BUILD:-$HERE/build-release}"
OUT="$HERE/build-tests"
mkdir -p "$OUT/classes" "$OUT/dex"
rm -rf "$OUT/classes" "$OUT/dex"
mkdir -p "$OUT/classes" "$OUT/dex"
javac --release 8 -Xlint:-options -classpath "$ANDROID_JAR:$TARGET/classes" -d "$OUT/classes" "$HERE"/java/dev/goblinlinux/sentry/*Acceptance.java
jar cf "$OUT/tests.jar" -C "$OUT/classes" .
"$BT/d8" --min-api 29 --lib "$ANDROID_JAR" --classpath "$TARGET/terminal.jar" --output "$OUT/dex" "$OUT/tests.jar"
cat > "$OUT/AndroidManifest.xml" <<'XML'
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="dev.goblinlinux.sentry.tests" android:versionCode="1" android:versionName="1">
  <application android:label="Goblin acceptance tests" android:hasCode="true" />
  <instrumentation android:name="dev.goblinlinux.sentry.ServicesAcceptance" android:targetPackage="dev.goblinlinux.sentry" />
  <instrumentation android:name="dev.goblinlinux.sentry.TerminalAcceptance" android:targetPackage="dev.goblinlinux.sentry" />
  <instrumentation android:name="dev.goblinlinux.sentry.PowerAcceptance" android:targetPackage="dev.goblinlinux.sentry" />
  <instrumentation android:name="dev.goblinlinux.sentry.ProductAcceptance" android:targetPackage="dev.goblinlinux.sentry" />
</manifest>
XML
mkdir -p "$OUT/assets"
cp "$HERE/../uml/build/artifacts/uml-parallel-test" "$OUT/assets/uml-parallel-test"
"$BT/aapt2" link --manifest "$OUT/AndroidManifest.xml" -I "$ANDROID_JAR" -A "$OUT/assets" --min-sdk-version 29 --target-sdk-version 36 -o "$OUT/unaligned.apk"
(cd "$OUT/dex" && zip -q "$OUT/unaligned.apk" classes.dex)
"$BT/zipalign" -f -P 16 4 "$OUT/unaligned.apk" "$OUT/aligned.apk"
python3 "$HERE/sign.py" "${SIGNING:-release}" "$OUT/aligned.apk" "$OUT/goblin-tests.apk" --build-tools "$BT"
rm "$OUT/aligned.apk" "$OUT/unaligned.apk"
echo "Built $OUT/goblin-tests.apk"
