#!/usr/bin/env bash
# Installs and runs the phase 1 harness, then prints its report.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APK="$HERE/build/goblin-sentry.apk"
PKG="dev.goblinlinux.sentry"
SERIAL="${1:-}"
ADB=(adb)
[ -n "$SERIAL" ] && ADB=(adb -s "$SERIAL")

[ -f "$APK" ] || { echo "error: no APK; run harness/build.sh first" >&2; exit 1; }

if [ "$("${ADB[@]}" get-state 2>/dev/null || true)" != "device" ]; then
    echo "error: no device." >&2
    "${ADB[@]}" devices -l >&2 || true
    exit 1
fi

echo "==> installing"
"${ADB[@]}" install -r -g "$APK" >/dev/null

echo "==> running"
"${ADB[@]}" logcat -c
"${ADB[@]}" shell am start -n "$PKG/android.app.NativeActivity" >/dev/null

completed=0
for _ in $(seq 1 60); do
    if "${ADB[@]}" logcat -d -s goblin-sentry:I 2>/dev/null | grep -q "PHASE1 COMPLETE"; then
        completed=1
        break
    fi
    sleep 1
done

if [ "$completed" -ne 1 ]; then
    echo "error: harness did not complete. Recent failures:" >&2
    "${ADB[@]}" logcat -d 2>/dev/null \
        | grep -iE "goblin|NativeActivity|AndroidRuntime|dlopen|avc: denied" \
        | tail -20 >&2
    exit 1
fi

mkdir -p "$HERE/results"
MODEL="$("${ADB[@]}" shell getprop ro.product.model | tr -d '\r' | tr ' /' '__')"
SDKV="$("${ADB[@]}" shell getprop ro.build.version.sdk | tr -d '\r')"
REPORT="$HERE/results/${MODEL}-api${SDKV}-$(date +%Y%m%d-%H%M%S).txt"

"${ADB[@]}" shell run-as "$PKG" cat files/phase1-report.txt 2>/dev/null \
    | tr -d '\r' > "$REPORT"

cat "$REPORT"
echo
echo "==> saved to $REPORT"
