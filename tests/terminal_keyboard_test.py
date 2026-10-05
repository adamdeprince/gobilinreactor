#!/usr/bin/env python3
"""Verify Android keyboard hotplug using an emulator's uinput test device.

Requires Goblin's terminal to be bootstrapped. Creates a temporary keyboard,
types through it, and disconnects it; no Android input settings are changed.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('serial')
parser.add_argument('--screenshot', type=Path)
args = parser.parse_args()
adb = ['adb', '-s', args.serial]
pkg = 'dev.goblinreactor.sentry'


def call(*command):
    return subprocess.check_output(adb + list(command), timeout=30).decode().strip()


def ui():
    call('shell', 'uiautomator', 'dump', '/data/local/tmp/goblin-keyboard.xml')
    return ET.fromstring(call('shell', 'cat', '/data/local/tmp/goblin-keyboard.xml'))


def wait(condition, name):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if condition():
            print('PASS:', name, flush=True)
            return
        time.sleep(0.25)
    raise AssertionError(name)


def has_keys():
    return any(n.attrib.get('text') == 'Esc' for n in ui().iter('node'))


call('shell', 'am', 'start', '-n', pkg + '/.TerminalActivity')
wait(has_keys, 'extra keys are visible without a physical keyboard')
wait(lambda: any('goblin@goblin:' in n.attrib.get('content-desc', '') for n in ui().iter('node')),
     'login shell is ready for keyboard input')
keyboard = subprocess.Popen(adb + ['shell', 'uinput', '-'], stdin=subprocess.PIPE,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)


def send(command):
    keyboard.stdin.write(json.dumps(dict(id=1, **command)) + '\n')
    keyboard.stdin.flush()


try:
    send(dict(command='register', name='Goblin acceptance keyboard', vid=0x18d2,
              pid=0x2c42, bus='usb', configuration=[
                  dict(type=100, data=[1]), dict(type=101, data=list(range(1, 89)))]))
    wait(lambda: not has_keys(), 'connecting a physical keyboard hides the extra keys')
    wait(lambda: any(n.attrib.get('content-desc') == 'Terminal menu' for n in ui().iter('node')),
         'terminal menu remains accessible with the extra keys hidden')
    # Actual Linux input events reach Android and then the guest PTY.
    codes = dict(zip('echo kybard', [18, 46, 35, 24, 57, 37, 21, 48, 30, 19, 32]))
    send(dict(command='updateTimeBase'))
    for char in 'echo keyboard\n':
        code = 28 if char == '\n' else codes[char]
        send(dict(command='inject', events=[1, code, 1, 0, 0, 0, 1, code, 0, 0, 0, 0]))
        send(dict(command='delay', duration=40))
    wait(lambda: any('\nkeyboard\n' in n.attrib.get('content-desc', '') for n in ui().iter('node')),
         'physical keyboard input executes in the terminal')
    if args.screenshot:
        args.screenshot.parent.mkdir(parents=True, exist_ok=True)
        args.screenshot.write_bytes(subprocess.check_output(adb + ['exec-out', 'screencap', '-p'], timeout=15))
finally:
    keyboard.stdin.close()
    try:
        keyboard.wait(timeout=10)
    except subprocess.TimeoutExpired:
        keyboard.terminate()
        keyboard.wait(timeout=10)
    errors = keyboard.stderr.read()
    if errors:
        print(errors)
wait(has_keys, 'disconnecting the physical keyboard restores the extra keys')
print('TERMINAL KEYBOARD PASS', flush=True)
