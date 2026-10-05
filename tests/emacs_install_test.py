#!/usr/bin/env python3
"""Install emacs-nox and exercise editing through the Android terminal.

Requires an unlocked device with Goblin's Debian environment already prepared.
Emacs remains installed. Other guest changes are limited to a unique test folder.
The separate `harness/run.sh SERIAL emacs` suite can repair and test the package
without an unlocked display.
"""
import argparse
from pathlib import Path
import re
import shlex
import subprocess
import time
import uuid
import xml.etree.ElementTree as ET
from uml_control import request

PKG = 'dev.goblinreactor.sentry'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('serial')
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
adb = ['adb', '-s', args.serial]
output = args.output
output.mkdir(parents=True, exist_ok=True)
work = '/home/goblin/goblin-emacs-' + uuid.uuid4().hex[:12]


def call(*command):
    return subprocess.check_output([*adb, *command], timeout=35).decode().strip()


def ui():
    call('shell', 'uiautomator', 'dump', '/data/local/tmp/goblin-emacs.xml')
    return ET.fromstring(call('shell', 'cat', '/data/local/tmp/goblin-emacs.xml'))


def screen():
    return '\n'.join(node.attrib.get('content-desc', '') for node in ui().iter('node')
                     if node.attrib.get('class') == 'android.view.SurfaceView')


def tap(node):
    left, top, right, bottom = map(int, re.findall(r'\d+', node.attrib['bounds']))
    call('shell', 'input', 'tap', str((left + right) // 2), str((top + bottom) // 2))


def command(text):
    view = next(node for node in ui().iter('node')
                if node.attrib.get('class') == 'android.view.SurfaceView')
    tap(view)
    ui()  # Let the IME finish changing the layout before injecting keys.
    for start in range(0, len(text), 32):
        call('shell', 'input', 'text', shlex.quote(text[start:start + 32].replace(' ', '%s')))
    call('shell', 'input', 'keyevent', '66')


def read(name):
    # The shell protocol preserves the remote exit status. exec-out can return
    # success with a missing-file diagnostic in stdout, which is not test data.
    result = request(adb, 'read', work + '/' + name, timeout=15)
    return result.stdout.decode() if result.returncode == 0 else None


def wait(condition, label, seconds=45):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if condition():
            print('PASS: ' + label, flush=True)
            return
        time.sleep(0.3)
    raise AssertionError(label + '; screen=' + screen())


def collect(name):
    data = read(name)
    if data is not None:
        (output / name).write_text(data)
    return data


def completed(name):
    return '\n' in (read(name + '.status') or '')


def control(letter):
    call('shell', 'input', 'keycombination', 'KEYCODE_CTRL_LEFT', 'KEYCODE_' + letter)


call('shell', 'am', 'start', '-n', PKG + '/.TerminalActivity')
wait(lambda: 'goblin@goblin:~$' in screen(), 'regular-user terminal ready')
probe = ('import os,json; s=os.statvfs("/"); '
         'print(json.dumps({"capacity":s.f_blocks*s.f_frsize,"available":s.f_bavail*s.f_frsize})); '
         'assert s.f_blocks*s.f_frsize > 2**30')
command('mkdir ' + work + '; python3 -c ' + shlex.quote(probe) + ' > ' + work +
        '/storage.json; echo $? > ' + work + '/storage.status')
wait(lambda: completed('storage'), 'guest storage reported')
assert read('storage.status') == '0\n'
collect('storage.json')
command('{ sudo -n apt -y --no-remove --fix-broken install && '
        'sudo -n apt -y --no-remove install emacs-nox; } > ' + work +
        '/install.log 2>&1; echo $? > ' + work + '/install.status')
wait(lambda: completed('install'), 'Emacs install command completed', 1200)
status = collect('install.status')
collect('install.log')
assert status == '0\n', (status, (output / 'install.log').read_text()[-5000:])
expression = '(princ (format "EMACS:%s\\n" emacs-version))'
command('dpkg --audit > ' + work + '/audit.txt; dpkg-query -W emacs-nox > ' + work +
        '/package.txt; emacs --batch -Q --eval ' + shlex.quote(expression) + ' > ' + work +
        '/batch.txt 2>&1; echo $? > ' + work + '/batch.status')
wait(lambda: completed('batch'), 'Emacs batch command completed', 90)
for name in ('audit.txt', 'package.txt', 'batch.txt', 'batch.status'):
    collect(name)
assert read('audit.txt') == ''
assert read('batch.status') == '0\n' and 'EMACS:' in read('batch.txt')
print('PASS: clean package audit and working Emacs batch mode', flush=True)
command('emacs -Q ' + work + '/edited.txt')
wait(lambda: 'edited.txt' in screen(), 'interactive Emacs opens a file', 60)
call('shell', 'input', 'text', 'Emacs%sedits%sinside%sGoblin.')
call('shell', 'input', 'keyevent', '66')
control('X')
control('S')
wait(lambda: read('edited.txt') == 'Emacs edits inside Goblin.\n', 'Emacs saves edited text')
(output / 'emacs.png').write_bytes(subprocess.check_output([*adb, 'exec-out', 'screencap', '-p'], timeout=30))
control('X')
control('C')
wait(lambda: screen().strip().endswith('goblin@goblin:~$'), 'Emacs exits to the original terminal')
command('rm -rf ' + work)
print('EMACS ACCEPTANCE PASS', flush=True)
