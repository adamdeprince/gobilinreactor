#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/goblin-runner.XXXXXX")"
trap 'rm -rf "$TEST_TMP"' EXIT
mkdir -p "$TEST_TMP/harness/build" "$TEST_TMP/bin"
cp "$ROOT/harness/run.sh" "$TEST_TMP/harness/run.sh"
touch "$TEST_TMP/harness/build/goblin-sentry.apk"
cat > "$TEST_TMP/bin/adb" <<'MOCK'
#!/usr/bin/env bash
case "$*" in
  get-state) echo device ;;
  'logcat -d -s goblin-sentry:I')
    if [ "$GOBLIN_TEST_CASE" != timeout ] && [ "$GOBLIN_TEST_CASE" != report_only ]; then echo 'GOBLIN COMPLETE'; fi ;;
  'shell run-as dev.goblinlinux.sentry test -s files/phase1-report.txt')
    [ "$GOBLIN_TEST_CASE" = report_only ] || exit 1 ;;
  'shell getprop ro.product.model') echo mocked-device ;;
  'shell getprop ro.build.version.sdk') echo 36 ;;
  'shell run-as dev.goblinlinux.sentry cat files/phase1-report.txt')
    case "$GOBLIN_TEST_CASE" in
      pass|report_only) printf 'GOBLIN PASS\r\n' ;;
      fail) echo 'GOBLIN FAIL' ;;
      contradiction) printf 'FAILED: test\nGOBLIN PASS\n' ;;
      empty) : ;;
      missing) exit 1 ;;
    esac ;;
esac
exit 0
MOCK
cat > "$TEST_TMP/bin/sleep" <<'MOCK'
#!/usr/bin/env bash
exit 0
MOCK
chmod +x "$TEST_TMP/bin/adb" "$TEST_TMP/bin/sleep"
for GOBLIN_TEST_CASE in pass report_only fail contradiction empty missing timeout; do
    export GOBLIN_TEST_CASE
    status=0
    PATH="$TEST_TMP/bin:$PATH" bash "$TEST_TMP/harness/run.sh" > "$TEST_TMP/output" 2>&1 || status=$?
    expected=1
    case "$GOBLIN_TEST_CASE" in pass|report_only) expected=0 ;; esac
    if { [ "$expected" -eq 0 ] && [ "$status" -ne 0 ]; } ||
       { [ "$expected" -ne 0 ] && [ "$status" -eq 0 ]; }; then
        cat "$TEST_TMP/output"
        echo "FAIL: case=$GOBLIN_TEST_CASE status=$status" >&2
        exit 1
    fi
done
echo 'harness runner: 7 regression cases passed'
