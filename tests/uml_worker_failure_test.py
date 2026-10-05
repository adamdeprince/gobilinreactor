#!/usr/bin/env python3
"""Emulator only: kill a disposable MM's host workers while it changes mappings.

Use the disposable emulator's su test shell to inspect the isolated UID; normal
app credentials cannot signal that UID. No device-wide policy is changed.
Read only Goblin mapping metadata. Identify the disposable MM by its marker
mapping and newly created host PIDs, so an existing shell or guest PID 1 cannot
become the target. Physical stub-data offsets can be reused after an MM exits.
"""
import argparse
import base64
import os
from pathlib import Path
import shlex
import subprocess
import time
import uuid

from uml_control import PACKAGE, request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('serial')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if not args.serial.startswith('emulator-'):
        parser.error('worker failure injection requires a disposable emulator')
    adb = [os.environ.get('ADB', 'adb'), '-s', args.serial]
    token = 'worker-failure-' + uuid.uuid4().hex
    source = '/tmp/' + token + '.c'
    binary = '/tmp/' + token
    report = []

    def shell(*command):
        return subprocess.check_output([*adb, 'shell', shlex.join(command)], timeout=20).decode().strip()

    def execute(command, timeout=30):
        r = request(adb, 'exec', command, timeout)
        if r.returncode:
            raise RuntimeError(r.stdout.decode(errors='replace') + r.stderr.decode(errors='replace'))
        return r.stdout.decode().replace('\r\n', '\n').strip()

    assert shell('su', '0', 'id', '-u') == '0', 'Requires a disposable userdebug emulator with its su test shell'
    hosts = [row.split() for row in shell('ps', '-A', '-o', 'UID,PID,PPID,NAME').splitlines()[1:]
             if PACKAGE + ':linux:' in row]
    assert len(hosts) == 1, 'Start exactly one isolated UML guest before this test'
    uid = hosts[0][0]

    def groups(signature=False):
        found = {}
        for row in shell('ps', '-A', '-o', 'UID,PID,PPID,NAME').splitlines()[1:]:
            fields = row.split()
            if len(fields) != 4 or fields[0] != uid or fields[3] != 'uml-userspace':
                continue
            pid = int(fields[1])
            try:
                maps = [line.split() for line in shell('su', '0', 'cat', f'/proc/{pid}/maps').splitlines()]
                if signature and not any(int(m[0].split('-')[0], 16) <= 0x1234000000 < int(m[0].split('-')[1], 16)
                                         for m in maps):
                    continue
                executable = [m for m in maps if 'x' in m[1] and len(m) > 5 and m[5] == '/memfd:goblin-uml-ram']
                code = max(executable, key=lambda m: int(m[0].split('-')[0], 16))
                data_start = code[0].split('-')[1]
                data = next(m for m in maps if m[0].split('-')[0] == data_start and m[1] == 'rw-s')
                key = (data[2], data[4])  # RAM offset, host memfd inode
                found.setdefault(key, []).append(pid)
            except (ValueError, StopIteration, subprocess.SubprocessError):
                continue  # An unrelated short-lived worker exited during the snapshot.
        return found

    try:
        encoded = base64.b64encode(Path(__file__).with_name('uml_parallel.c').read_bytes()).decode()
        execute('umask 077; printf %s ' + shlex.quote(encoded) + ' | base64 -d > ' + shlex.quote(source))
        execute(f'gcc -O2 -std=gnu11 -pthread {source} -o {binary}', 120)
        boot = execute('cat /proc/sys/kernel/random/boot_id')
        # Exercise both the dedicated mapper and an execution worker, repeatedly.
        for round_number in range(6):
            assert not groups(signature=True), 'a prior marker MM is still alive'
            before = groups()
            existing_pids = {pid for pids in before.values() for pid in pids}
            # Keep the creating PTY alive until setsid has detached the child.
            execute(f'setsid {binary} --map-loop </dev/null >{binary}.log 2>&1 &\n'
                    f'while ! grep -q "^READY " {binary}.log 2>/dev/null; do sleep .05; done', 20)
            ready = ''
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                ready = execute(f'cat {binary}.log')
                if ready.startswith('READY '):
                    break
                time.sleep(.1)
            assert ready.startswith('READY '), ready
            guest_pid = int(ready.split()[1])
            after = groups(signature=True)
            new = {key: pids for key, pids in after.items()
                   if len(pids) >= 3 and not existing_pids.intersection(pids)}
            assert len(new) == 1, (before, after, new)
            key, pids = next(iter(new.items()))
            # The mapper exists before it clones any execution workers.
            victim = min(pids) if round_number % 2 == 0 else max(pids)
            shell('su', '0', 'kill', '-KILL', str(victim))
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                state = execute(f'if [ -r /proc/{guest_pid}/stat ]; then cat /proc/{guest_pid}/stat; fi', 10)
                if not state:
                    break
                time.sleep(.1)
            assert not state, 'affected guest process survived worker death: ' + state
            assert execute('cat /proc/sys/kernel/random/boot_id', 10) == boot, 'VM restarted'
            remaining = groups()
            remaining_pids = {pid for workers in remaining.values() for pid in workers}
            assert not remaining_pids.intersection(pids), 'worker resources were not reaped'
            assert not groups(signature=True), 'marker MM was not reaped'
            report.append(f'PASS: round {round_number + 1}, killed host {victim}, guest {guest_pid} terminated, MM reaped, same VM responsive\n')
            print(report[-1], end='', flush=True)
        report.append('GOBLIN WORKER FAILURE PASS\n')
        print(report[-1], end='')
    finally:
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(''.join(report))
        try:
            # Normally each injected death has already killed the test. On an
            # assertion failure, match the unique executable before cleanup.
            execute(f'pkill -f "^{binary} --map-loop$" || true; rm -f {binary} {binary}.log {source}', 10)
        except (OSError, RuntimeError, subprocess.SubprocessError):
            print('Cleanup incomplete:', binary, source)


if __name__ == '__main__':
    main()
