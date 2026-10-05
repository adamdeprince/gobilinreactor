#!/usr/bin/env python3
"""Run the complete acceptance matrix on connected 4 KiB and 16 KiB devices.

Both devices must be unlocked for the Android UI checks. Existing Debian roots
are preserved; suites create and remove their own test fixtures. Installed tools
remain available in the terminal. Reports include the exact APK SHA256.
"""
import argparse
import concurrent.futures
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PKG = 'dev.goblinlinux.sentry'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('serial', nargs='+', help='ADB serials covering both page sizes')
    parser.add_argument('--apk', type=Path, default=ROOT / 'harness/build/goblin-sentry.apk')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    now = datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S')
    output = args.output or ROOT / 'harness/results' / ('matrix-' + now)
    output.mkdir(parents=True, exist_ok=True)
    devices = []
    for serial in args.serial:
        adb = ['adb', '-s', serial]
        def shell(*command):
            return subprocess.check_output([*adb, 'shell', *command], text=True, timeout=30).strip()
        devices.append(dict(serial=serial, page_size=int(shell('getconf', 'PAGESIZE')),
                            model=shell('getprop', 'ro.product.model'), api=shell('getprop', 'ro.build.version.sdk')))
    if not {4096, 16384}.issubset({d['page_size'] for d in devices}):
        parser.error('The complete matrix requires both 4096-byte and 16384-byte devices')
    with args.apk.open('rb') as stream:
        apk_sha = hashlib.file_digest(stream, 'sha256').hexdigest()
    report = dict(started_utc=now, apk_sha256=apk_sha, devices=devices, passed=False)

    def device_run(device, apk):
        adb = ['adb', '-s', device['serial']]
        directory = output / device['serial']
        directory.mkdir(exist_ok=True)
        device['checks'] = []
        device['metrics'] = []
        env = dict(os.environ, GOBLIN_APK=str(apk))

        def check(name, command, token, timeout):
            print(f"{device['serial']}: {name}", flush=True)
            with (directory / (name + '.txt')).open('w') as log:
                try:
                    result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                            env=env, cwd=ROOT, timeout=timeout)
                    status = result.returncode
                except subprocess.TimeoutExpired:
                    log.write('\nFAILED: host timeout\n')
                    status = -1
            text = (directory / (name + '.txt')).read_text()
            passed = status == 0 and token in text and 'FAILED:' not in text
            device['checks'].append(dict(name=name, passed=passed, exit_status=status))
            for line in text.splitlines():
                if line.startswith('METRIC '):
                    device['metrics'].append(json.loads(line[7:]))
            if not passed:
                raise RuntimeError(f"{device['serial']} failed {name}; see {directory / (name + '.txt')}")

        for mode in ('regression', 'persistent', 'developer', 'recovery'):
            check(mode, ['bash', 'harness/run.sh', device['serial'], mode], 'GOBLIN PASS', 2700)
        check('kitty', [*adb, 'shell', 'am', 'instrument', '-w', '-r',
                       PKG + '/.TerminalAcceptance'], 'KITTY ACCEPTANCE PASS', 240)
        screenshot = subprocess.check_output([*adb, 'exec-out', 'run-as', PKG, 'cat',
                                              'files/terminal-acceptance.png'], timeout=30)
        (directory / 'kitty.png').write_bytes(screenshot)
        check('lifecycle', ['python3', 'tests/terminal_lifecycle_test.py', device['serial'],
                            '--screenshot', str((directory / 'lifecycle.png').resolve())],
              'TERMINAL LIFECYCLE PASS', 240)
        device['passed'] = True

    # A stable copy prevents a concurrent harness build from removing the APK
    # while adb reads it. Devices run independently; modes on each are serial.
    errors = []
    with tempfile.TemporaryDirectory(prefix='goblin-matrix-') as directory:
        apk = Path(directory) / 'acceptance.apk'
        shutil.copy2(args.apk, apk)
        with concurrent.futures.ThreadPoolExecutor(len(devices)) as pool:
            futures = [pool.submit(device_run, device, apk) for device in devices]
            for future in futures:
                try:
                    future.result()
                except Exception as error:
                    errors.append(str(error))
    report['passed'] = not errors
    report['errors'] = errors
    report['finished_utc'] = datetime.now(timezone.utc).isoformat()
    (output / 'matrix.json').write_text(json.dumps(report, indent=2) + '\n')
    print(('PASS' if report['passed'] else 'FAIL') + ': ' + str(output / 'matrix.json'))
    if errors:
        raise SystemExit('\n'.join(errors))


if __name__ == '__main__':
    main()
