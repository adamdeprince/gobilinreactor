#!/usr/bin/env bash
# Installs and runs the phase 1 harness, then prints its report.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APK="${GOBLIN_APK:-$HERE/build/goblin-sentry.apk}"
PKG="dev.goblinreactor.sentry"
SERIAL="${1:-}"
MODE="${2:-regression}"
case "$MODE" in regression|persistent|developer|recovery|emacs) ;; *) echo "usage: harness/run.sh [serial] [regression|persistent|developer|recovery|emacs]" >&2; exit 2 ;; esac
MODE_ID=0
WAIT_SECONDS=180
if [ "$MODE" = persistent ]; then MODE_ID=1; WAIT_SECONDS=2400; fi
if [ "$MODE" = developer ]; then MODE_ID=2; WAIT_SECONDS=2400; fi
if [ "$MODE" = recovery ]; then MODE_ID=3; WAIT_SECONDS=600; fi
if [ "$MODE" = emacs ]; then MODE_ID=4; WAIT_SECONDS=2400; fi
ADB=(adb)
[ -n "$SERIAL" ] && ADB=(adb -s "$SERIAL")

[ -f "$APK" ] || { echo "error: no APK; run harness/build.sh first" >&2; exit 1; }

if [ "$("${ADB[@]}" get-state 2>/dev/null || true)" != "device" ]; then
    echo "error: no device." >&2
    "${ADB[@]}" devices -l >&2 || true
    exit 1
fi

echo "==> installing"
"${ADB[@]}" install --no-streaming -r -g "$APK"
if [ "$MODE" = recovery ]; then
    if [ -z "$SERIAL" ]; then SERIAL="$("${ADB[@]}" get-serialno)"; fi
    exec python3 "$HERE/../tests/uml_recovery_test.py" "$SERIAL"
fi

echo "==> running"
"${ADB[@]}" logcat -c
"${ADB[@]}" shell am force-stop "$PKG"
"${ADB[@]}" shell run-as "$PKG" rm -f files/phase1-report.txt files/phase1-report.txt.new
"${ADB[@]}" shell am start -n "$PKG/.HarnessActivity" --ei mode "$MODE_ID" >/dev/null

completed=0
restarted=0
for _ in $(seq 1 "$WAIT_SECONDS"); do
    if [ "$MODE" = recovery ] && [ "$restarted" -eq 0 ] &&
       "${ADB[@]}" shell run-as "$PKG" test -s files/recovery-restart 2>/dev/null; then
        echo "==> restarting after the deliberate Android process kill"
        "${ADB[@]}" shell am force-stop "$PKG"
        "${ADB[@]}" shell run-as "$PKG" rm -f files/recovery-restart
        "${ADB[@]}" shell am start -n "$PKG/.HarnessActivity" --ei mode 3 >/dev/null
        restarted=1
    fi
    if "${ADB[@]}" logcat -d -s goblin-sentry:I 2>/dev/null | grep -q "GOBLIN COMPLETE" ||
       "${ADB[@]}" shell run-as "$PKG" test -s files/phase1-report.txt 2>/dev/null; then
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
REPORT="$HERE/results/${MODEL}-${MODE}-api${SDKV}-$(date +%Y%m%d-%H%M%S).txt"

"${ADB[@]}" shell run-as "$PKG" cat files/phase1-report.txt 2>/dev/null \
    | tr -d '\r' > "$REPORT"

cat "$REPORT"
echo
echo "==> saved to $REPORT"
if ! grep -qx 'GOBLIN PASS' "$REPORT" || grep -qE 'FAILED|GOBLIN FAIL' "$REPORT"; then
    echo "error: harness reported failure or no pass verdict" >&2
    exit 1
fi
