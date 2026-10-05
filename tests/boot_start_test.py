#!/usr/bin/env python3
"""Exercise opt-in startup across real reboots of a disposable ARM64 emulator.

Uses emulator ADB root to observe the release app's private control socket;
the app stays unprivileged. Never run on a personal phone. No data is cleared.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time
import uuid
import xml.etree.ElementTree as ET

PACKAGE = 'dev.goblinreactor.sentry'
DATA = '/data/user/0/' + PACKAGE
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('serial')
p.add_argument('apk', type=Path)
p.add_argument('--output', type=Path, required=True)
args = p.parse_args()
if not args.serial.startswith('emulator-'):
    p.error('This test reboots its target; use a disposable emulator')
adb = [os.environ.get('ADB', 'adb'), '-s', args.serial]
args.output.mkdir(parents=True, exist_ok=True)
checks = []

def run(*command, timeout=40):
    return subprocess.check_output([*adb, *command], text=True, stderr=subprocess.STDOUT, timeout=timeout).strip()

def shell(*command, timeout=40):
    return run('shell', shlex.join(command), timeout=timeout)

def wait(check, description, seconds=120):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            value = check()
            if value:
                return value
        except (subprocess.SubprocessError, ET.ParseError, OSError):
            pass
        time.sleep(1)
    raise AssertionError(description)

def passed(description):
    checks.append(description)
    print('PASS:', description, flush=True)

def root():
    run('root')
    run('wait-for-device')
    assert shell('id', '-u') == '0'

def guest(command):
    control = shell('cat', DATA + '/files/uml/ctl-path')
    return shell(control, DATA + '/files/uml/control.sock', 'exec', command, timeout=15)

def kernel_running():
    services = shell('dumpsys', 'activity', 'services', PACKAGE)
    return any('ServiceRecord' in line and '.KernelService' in line for line in services.splitlines())

def ready():
    wait(lambda: guest('printf READY') == 'READY', 'Guest startup', 180)

def ui():
    shell('uiautomator', 'dump', '/data/local/tmp/goblin-boot-test.xml')
    return ET.fromstring(shell('cat', '/data/local/tmp/goblin-boot-test.xml'))

def tap(node):
    x1, y1, x2, y2 = map(int, re.findall(r'\d+', node.get('bounds')))
    shell('input', 'tap', str((x1+x2)//2), str((y1+y2)//2))

def click(label):
    for attempt in range(5):
        tree = ui()
        for node in tree.iter('node'):
            if label.casefold() == node.get('text', '').casefold() or label == node.get('content-desc'):
                tap(node)
                return
        scroll = next((n for n in tree.iter('node') if n.get('scrollable') == 'true'), None)
        if scroll is None:
            raise AssertionError('Missing UI item: ' + label)
        x1, y1, x2, y2 = map(int, re.findall(r'\d+', scroll.get('bounds')))
        shell('input', 'swipe', str((x1+x2)//2), str(y2-40), str((x1+x2)//2), str(y1+40), '300')
    raise AssertionError('Menu item not found: ' + label)

def open_app():
    shell('input', 'keyevent', '224')
    shell('wm', 'dismiss-keyguard')
    shell('am', 'start', '-n', PACKAGE + '/.TerminalActivity')
    ready()

def preference(name, key, default=False):
    try:
        tree = ET.fromstring(shell('cat', DATA + '/shared_prefs/' + name + '.xml'))
    except subprocess.CalledProcessError:
        return default
    node = next((n for n in tree if n.get('name') == key), None)
    return default if node is None else node.get('value') == 'true'

def boot_setting(value):
    if preference('linux-startup', 'after-reboot') == value:
        return
    open_app()
    click('Terminal menu')
    click('Start environment after reboot')
    if value:
        click('Enable')
    wait(lambda: preference('linux-startup', 'after-reboot') == value, 'Boot preference persisted')

def wake_setting(value):
    if preference('linux-power', 'keep-awake', True) == value:
        return
    open_app()
    click('Terminal menu')
    click('Keep environment awake')
    wait(lambda: preference('linux-power', 'keep-awake', True) == value, 'Wake preference persisted')

def stop():
    if kernel_running():
        guest('sync; systemctl poweroff')
        wait(lambda: not kernel_running(), 'Clean guest shutdown')

def reboot():
    stop()
    shell('input', 'keyevent', '3')
    before = shell('cat', '/proc/sys/kernel/random/boot_id')
    run('reboot')
    run('wait-for-device', timeout=180)
    wait(lambda: shell('getprop', 'sys.boot_completed') == '1', 'Android reboot', 180)
    root()
    assert shell('cat', '/proc/sys/kernel/random/boot_id') != before
    wait(lambda: shell('am', 'get-started-user-state', '0') == 'RUNNING_UNLOCKED', 'First user unlock')

root()
stop()
run('install', '-r', str(args.apk), timeout=180)
open_app()  # Normal use clears Android's stopped-package state.
assert not preference('linux-startup', 'after-reboot'), 'Use an emulator with the setting initially off'
original_awake = preference('linux-power', 'keep-awake', True)
token = uuid.uuid4().hex
unit = 'goblin-boot-test-' + token + '.service'
marker = '/home/goblin/.goblin-boot-test-' + token
unit_path = '/etc/systemd/system/' + unit
try:
    reboot()
    time.sleep(10)
    assert not kernel_running()
    passed('Default-off setting leaves Linux stopped after a real reboot')

    open_app()
    config = ('[Unit]\nDescription=Goblin reboot acceptance\n[Service]\nType=oneshot\n'
              'ExecStart=/bin/sh -c "cat /proc/sys/kernel/random/boot_id >> ' + marker + '"\n'
              '[Install]\nWantedBy=multi-user.target\n')
    guest('printf %s ' + shlex.quote(config) + ' > ' + shlex.quote(unit_path)
          + '; systemctl daemon-reload; systemctl enable ' + shlex.quote(unit) + '; sync')
    boot_setting(True)
    wake_setting(False)
    reboot()
    ready()
    first = guest('cat ' + shlex.quote(marker)).splitlines()
    assert len(first) == 1
    top = shell('dumpsys', 'activity', 'activities')
    assert not any(PACKAGE in line and ('mResumedActivity' in line or 'topResumedActivity' in line) for line in top.splitlines())
    services = shell('dumpsys', 'activity', 'services', PACKAGE)
    assert 'isForeground=true' in services
    assert not re.search(r"PARTIAL_WAKE_LOCK\s+'Goblin:Linux'", shell('dumpsys', 'power'))
    passed('Opt-in survives a real reboot and starts Linux with a foreground notification without opening the terminal')
    passed('Enabled guest systemd service runs automatically; Keep environment awake opt-out remains respected')

    guest_before = guest('cat /proc/sys/kernel/random/boot_id')
    shell('am', 'broadcast', '-a', 'android.intent.action.BOOT_COMPLETED', '-p', PACKAGE)
    time.sleep(3)
    assert guest('cat /proc/sys/kernel/random/boot_id') == guest_before
    assert guest('cat ' + shlex.quote(marker)).splitlines() == first
    passed('Repeated boot notification retains the same Linux instance and guest service execution')

    boot_setting(False)
    assert kernel_running()
    passed('Disabling startup leaves the current Linux instance running')
    wake_setting(original_awake)
    reboot()
    time.sleep(10)
    assert not kernel_running()
    passed('Disabling the setting prevents startup on the following real reboot')

    open_app()
    second = guest('cat ' + shlex.quote(marker)).splitlines()
    assert len(second) == 2 and first[0] != second[1]
    guest('systemctl disable ' + shlex.quote(unit) + '; rm -f ' + shlex.quote(unit_path) + ' ' + shlex.quote(marker) + '; systemctl daemon-reload; sync')
    passed('Manual opening still starts Linux; guest files persist and test service is removed')
    result = {'result':'PASS', 'serial':args.serial, 'api':shell('getprop','ro.build.version.sdk'),
              'host_page_size':shell('getconf','PAGESIZE'), 'real_reboots':3,
              'apk_sha256':hashlib.sha256(args.apk.read_bytes()).hexdigest(), 'checks':checks,
              'scope':'User unlocked automatically; no PIN-locked pre-unlock test. Emulator root observes the release app; app privileges are unchanged.'}
    (args.output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2), flush=True)
finally:
    # Leave the optional feature off even if an assertion fails.
    try:
        if kernel_running():
            boot_setting(False)
            wake_setting(original_awake)
            guest('systemctl disable ' + shlex.quote(unit) + ' 2>/dev/null; rm -f ' + shlex.quote(unit_path) + ' ' + shlex.quote(marker) + '; systemctl daemon-reload; sync')
    except (subprocess.SubprocessError, AssertionError):
        pass
