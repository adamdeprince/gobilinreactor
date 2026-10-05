#!/usr/bin/env bash
# Reproducible ADB deployment. Ordinary APK installs retain Linux data.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERIAL="${1:?usage: deploy.sh SERIAL [APK]}"
APK="${2:-$HERE/build-release/goblin-sentry.apk}"
ADB=("${ADB:-adb}" -s "$SERIAL")
[ "$#" -le 2 ] || { echo 'usage: deploy.sh SERIAL [APK]' >&2; exit 2; }
"${ADB[@]}" install -r "$APK"
"${ADB[@]}" shell am start -n dev.goblinreactor.sentry/.TerminalActivity
