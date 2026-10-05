#!/usr/bin/env python3
"""Run full-guest acceptance while observing Android hosting and unchanged policy.

Requires the matching separate test APK. Never changes monitoring, SELinux,
battery policy, or the user's Linux disk. The acceptance workload cleans up its
own uniquely named files and services.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time

PACKAGE = 'dev.goblinreactor.sentry'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('serial')
p.add_argument('--mode', choices=['soak', 'network', 'recovery', 'transport'], default='soak')
p.add_argument('--seconds', type=int, default=600)
p.add_argument('--output', type=Path, required=True)
args = p.parse_args()
if args.seconds <= 0:
    p.error('seconds must be positive')
args.output.mkdir(parents=True, exist_ok=True)
adb = [os.environ.get('ADB', str(Path.home() / 'Library/Android/sdk/platform-tools/adb')), '-s', args.serial]


def shell(*command):
    return subprocess.check_output([*adb, 'shell', *command], text=True, timeout=30)


def policy():
    return {
        'monitor_setting': shell('settings', 'get', 'global', 'settings_enable_monitor_phantom_procs').strip(),
        'monitor_property': shell('getprop', 'persist.sys.fflag.override.settings_enable_monitor_phantom_procs').strip(),
        'phantom_limit': [line.strip() for line in shell('dumpsys', 'activity', 'settings').splitlines() if 'max_phantom_processes=' in line],
        'selinux': shell('getenforce').strip(),
    }


metadata = dict(serial=args.serial, mode=args.mode, seconds=args.seconds, before=policy(),
                fingerprint=shell('getprop', 'ro.build.fingerprint').strip(),
                host_page_size=int(shell('getconf', 'PAGESIZE').strip()),
                hardware_cpu_mask=shell('cat', '/sys/devices/system/cpu/present').strip())
assert metadata['before']['monitor_setting'] != 'false'
assert metadata['before']['monitor_property'] != 'false'
assert metadata['before']['selinux'] == 'Enforcing'
samples, worker_pids = [], set()
started = time.monotonic()
command = [*adb, 'shell', 'am', 'instrument', '-w', '-e', 'mode', args.mode, '-e', 'seconds', str(args.seconds),
           PACKAGE + '.tests/' + PACKAGE + '.ServicesAcceptance']
with (args.output / 'instrumentation.txt').open('w') as log:
    process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
    try:
        while process.poll() is None:
            if time.monotonic() - started > max(1200, args.seconds + 300):
                raise RuntimeError('Acceptance exceeded its deadline')
            rows = [row.split(None, 4) for row in shell('ps', '-A', '-o', 'PID,PPID,UID,NAME,CMD').splitlines()[1:]]
            # Android's CMD may be the 15-byte comm, while NAME is the full
            # process name. Devices differ; inspect both exported columns.
            hosts = [row for row in rows if len(row) == 5 and PACKAGE + ':linux' in ' '.join(row[3:])]
            uids = {row[2] for row in hosts}
            members = [row for row in rows if len(row) == 5 and row[2] in uids]
            workers = [row for row in members if 'uml-userspace' in row[3]]
            worker_pids.update(row[0] for row in workers)
            activity = shell('dumpsys', 'activity', 'processes')
            tracked = [line.strip() for line in activity.splitlines() if 'PhantomProcessRecord' in line and
                       (PACKAGE in line or 'uml-userspace' in line or any(':' + row[0] + ':' in line for row in members))]
            sample = dict(elapsed=round(time.monotonic() - started, 1), hosts=hosts, workers=len(workers),
                          processes=members, tracked_phantoms=tracked)
            samples.append(sample)
            (args.output / 'samples.json').write_text(json.dumps(samples, indent=2) + '\n')
            print(f'{args.serial} {args.mode}: {sample["elapsed"]}s, workers={len(workers)}, tracked={len(tracked)}', flush=True)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
    except BaseException:
        # Stop only this instrumentation/app; never alter global policy.
        shell('am', 'force-stop', PACKAGE)
        process.wait(timeout=30)
        raise
metadata.update(after=policy(), duration_seconds=round(time.monotonic() - started, 1), adb_returncode=process.returncode)
output = (args.output / 'instrumentation.txt').read_text()
metadata['acceptance_passed'] = 'GOBLIN SERVICES PASS' in output and 'FAIL:' not in output
metadata['policy_unchanged'] = metadata['before'] == metadata['after']
metadata['peak_workers'] = max((sample['workers'] for sample in samples), default=0)
metadata['tracked_phantoms'] = sorted({line for sample in samples for line in sample['tracked_phantoms']})
remaining = {row.split()[0] for row in shell('ps', '-A', '-o', 'PID').splitlines()[1:]}
metadata['workers_remaining_after_stop'] = sorted(worker_pids & remaining)
logs = shell('logcat', '-d', '-s', 'ActivityManager:I')
(args.output / 'activity-log.txt').write_text('\n'.join(line for line in logs.splitlines() if re.search(r'goblinreactor|uml-userspace', line)) + '\n')
(args.output / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
print('\n'.join(output.splitlines()[-25:]))
print(json.dumps(metadata, indent=2))
assert metadata['acceptance_passed'] and metadata['policy_unchanged']
assert metadata['peak_workers'] > 0, 'No isolated guest workers observed'
assert not metadata['tracked_phantoms'], 'Managed guest appeared in phantom accounting'
assert not metadata['workers_remaining_after_stop'], 'Workers survived host termination'
