#!/usr/bin/env python3
"""Run real-device acceptance and an in-place debug-to-release signing upgrade.

Build both APK variants and the matching test APKs first. No uninstall, data
clear, USB power simulation or guest filesystem replacement is performed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('serial')
p.add_argument('--release', type=Path, required=True)
p.add_argument('--tests', type=Path, required=True, help='Release-key test APK')
p.add_argument('--debug-tests', type=Path, help='Old-key test APK, for the record step before first signing rotation')
p.add_argument('--output', type=Path, required=True)
p.add_argument('--soak-seconds', type=int, default=600)
p.add_argument('--ui', action='store_true', help='Also run terminal rendering; leave device unlocked')
args = p.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
adb = [os.environ.get('ADB', 'adb'), '-s', args.serial]
component = 'dev.goblinlinux.sentry.tests/dev.goblinlinux.sentry.'

def run(command, name, timeout=1200):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    (args.output / (name + '.txt')).write_bytes(result.stdout)
    if result.returncode: raise RuntimeError(name + ' failed: ' + result.stdout.decode(errors='replace'))
    return result.stdout.decode(errors='replace')

def test(mode, klass='ServicesAcceptance', extra=()):
    print(args.serial, mode, flush=True)
    output = run([*adb, 'shell', 'am', 'instrument', '-w', '-e', 'mode', mode, *extra, component + klass], mode,
                 max(1200, args.soak_seconds + 300))
    verdict = 'GOBLIN SERVICES PASS' if klass == 'ServicesAcceptance' else 'GOBLIN POWER PASS' if klass == 'PowerAcceptance' else 'KITTY ACCEPTANCE PASS'
    if verdict not in output or 'FAIL:' in output: raise RuntimeError(mode + ': ' + output)

if args.debug_tests: run([*adb, 'install', '-r', str(args.debug_tests)], 'install-debug-tests', 180)
else: run([*adb, 'install', '-r', str(args.tests)], 'install-tests', 180)
test('upgrade-record')
run([*adb, 'install', '-r', str(args.release)], 'install-release', 180)
run([*adb, 'install', '-r', str(args.tests)], 'install-release-tests', 180)
test('upgrade-verify')
test('transport')
test('network')
test('recovery')
test('soak', extra=('-e', 'seconds', str(args.soak_seconds)))
test('power', 'PowerAcceptance')
if args.ui: test('terminal', 'TerminalAcceptance')
report = {'result':'PASS', 'serial':args.serial, 'finished':time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
          'apk_sha256':hashlib.sha256(args.release.read_bytes()).hexdigest(), 'soak_seconds':args.soak_seconds,
          'ui':args.ui, 'physical_unplugged_test':'separate ServicesAcceptance unplugged mode; not simulated here'}
(args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
