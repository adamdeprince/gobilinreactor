#!/usr/bin/env bash
# Installs, runs and collects the goblin-linux capability probe.
#
# Must be a real device. An emulator answers a different question: its kernel and
# SELinux policy are not the ones the app will ship against, and the primitives
# this probe cares about (memfd exec permission, userfaultfd, page size) are
# exactly the ones that differ.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APK="$HERE/build/goblin-probe.apk"
PKG="dev.goblinlinux.probe"
SERIAL="${1:-}"
ADB=(adb)
[ -n "$SERIAL" ] && ADB=(adb -s "$SERIAL")

[ -f "$APK" ] || { echo "error: no APK; run probe/build.sh first" >&2; exit 1; }

if [ "$("${ADB[@]}" get-state 2>/dev/null || true)" != "device" ]; then
    echo "error: no device. Connect one with USB debugging enabled." >&2
    "${ADB[@]}" devices -l >&2 || true
    exit 1
fi

MODEL="$("${ADB[@]}" shell getprop ro.product.model | tr -d '\r' | tr ' /' '__')"
RELEASE="$("${ADB[@]}" shell getprop ro.build.version.release | tr -d '\r')"
SDKV="$("${ADB[@]}" shell getprop ro.build.version.sdk | tr -d '\r')"
echo "==> device: $MODEL, Android $RELEASE (API $SDKV)"

echo "==> installing"
"${ADB[@]}" install -r -g "$APK" >/dev/null

echo "==> running"
"${ADB[@]}" logcat -c
"${ADB[@]}" shell am start -n "$PKG/android.app.NativeActivity" >/dev/null

for _ in $(seq 1 60); do
    if "${ADB[@]}" logcat -d -s goblin-probe:I 2>/dev/null | grep -q "PROBE COMPLETE"; then
        break
    fi
    sleep 1
done

mkdir -p "$HERE/results"
REPORT="$HERE/results/${MODEL}-api${SDKV}-$(date +%Y%m%d-%H%M%S).txt"

{
    echo "device:  $MODEL"
    echo "android: $RELEASE (API $SDKV)"
    echo "abi:     $("${ADB[@]}" shell getprop ro.product.cpu.abi | tr -d '\r')"
    echo "kernel:  $("${ADB[@]}" shell uname -a | tr -d '\r')"
    echo
    # run-as works because the probe is built debuggable.
    "${ADB[@]}" shell run-as "$PKG" cat files/probe-report.txt 2>/dev/null \
        || "${ADB[@]}" logcat -d -s goblin-probe:I
} | tr -d '\r' > "$REPORT"

echo
cat "$REPORT"
echo
echo "==> saved to $REPORT"
