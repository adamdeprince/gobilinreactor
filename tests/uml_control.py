#!/usr/bin/env python3
"""Read or execute inside the running UML guest through its private app socket."""
import os
import shlex
import subprocess

PACKAGE = 'dev.goblinreactor.sentry'

def request(adb, operation, argument, timeout=30):
    path = subprocess.check_output([*adb, 'shell', 'run-as', PACKAGE, 'cat',
                                    'files/uml/ctl-path'], timeout=15).decode().strip()
    command = ['run-as', PACKAGE, path, 'files/uml/control.sock', operation, argument]
    return subprocess.run([*adb, 'shell', shlex.join(command)], capture_output=True, timeout=timeout)

if __name__ == '__main__':
    import argparse
    import sys
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('serial'); p.add_argument('operation', choices=['read','exec','ports'])
    p.add_argument('argument'); p.add_argument('--timeout', type=int, default=300)
    args = p.parse_args()
    r = request([os.environ.get('ADB', 'adb'), '-s', args.serial], args.operation, args.argument, args.timeout)
    sys.stdout.buffer.write(r.stdout); sys.stderr.buffer.write(r.stderr); sys.exit(r.returncode)
