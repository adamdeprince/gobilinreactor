#!/usr/bin/env python3
"""Match hardware CPUs and run native pthread/MM regressions in a running APK."""
import argparse
import base64
import os
import re
from pathlib import Path
import shlex
import subprocess
import uuid

from uml_control import PACKAGE, request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('serial')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--binary', type=Path, help='Use a prebuilt static ARM64 test for a fresh guest without GCC')
    args = parser.parse_args()
    adb = [os.environ.get('ADB', 'adb'), '-s', args.serial]
    token = 'parallel-' + uuid.uuid4().hex
    source = Path(__file__).with_name('uml_parallel.c')
    guest_source = '/tmp/' + token + '.c'
    binary = '/tmp/' + token
    report = []

    def shell(*command):
        return subprocess.check_output([*adb, 'shell', shlex.join(command)], timeout=30).decode().strip()

    def execute(command, timeout=30):
        r = request(adb, 'exec', command, timeout)
        output = r.stdout.decode(errors='replace').replace('\r\n', '\n')
        if r.returncode:
            report.append(output + r.stderr.decode(errors='replace'))
            raise RuntimeError('guest command failed: ' + command + '\n' + output)
        return output

    def cpu_count(mask):
        total = 0
        for item in mask.split(','):
            ends = list(map(int, item.split('-')))
            total += 1 if len(ends) == 1 else ends[1] - ends[0] + 1
        return total

    try:
        hardware = shell('cat', '/sys/devices/system/cpu/present')
        configured = execute('getconf _NPROCESSORS_CONF').strip()
        online = execute('getconf _NPROCESSORS_ONLN').strip()
        assert int(configured) == int(online) == cpu_count(hardware), (hardware, configured, online)
        cpuinfo = execute('cat /proc/cpuinfo')
        assert len(re.findall(r'^processor\s*:', cpuinfo, re.MULTILINE)) == int(online), cpuinfo
        report.append(execute('uname -a'))
        report.append(f'PASS: Android hardware {hardware}; guest configured={configured}, online={online}\n')
        # Boot assets are immutable for a running VM. Transfer test material
        # through the private guest control channel instead of live hostfs.
        execute('umask 077; : > ' + shlex.quote(guest_source))
        payload = (args.binary or source).read_bytes()
        for offset in range(0, len(payload), 32768):
            encoded = base64.b64encode(payload[offset:offset + 32768]).decode()
            execute('printf %s ' + shlex.quote(encoded) + ' | base64 -d >> ' + shlex.quote(guest_source))
        if args.binary:
            execute('cp ' + shlex.quote(guest_source) + ' ' + shlex.quote(binary) + '; chmod 700 ' + shlex.quote(binary))
        else:
            execute('gcc -O2 -std=gnu11 -pthread -Wall -Wextra ' + shlex.quote(guest_source) + ' -o ' + shlex.quote(binary), 120)
        print('Running native parallelism, TLS, memory protection, migration and fork regressions.', flush=True)
        output = execute('timeout 180 ' + shlex.quote(binary), 200)
        report.append(output)
        assert 'GOBLIN PARALLEL PASS' in output
        print(''.join(report), end='')
    finally:
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(''.join(report))
        try:
            execute('rm -f ' + shlex.quote(binary) + ' ' + shlex.quote(guest_source), 10)
        except (OSError, RuntimeError, subprocess.SubprocessError):
            print('Test cleanup incomplete:', binary, guest_source)


if __name__ == '__main__':
    main()
