#!/usr/bin/env python3
"""Force-stop a dirty UML VM and verify journal recovery and synced user data."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import time
import uuid
from uml_control import PACKAGE, request

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('serial'); args=p.parse_args()
adb=[os.environ.get('ADB','adb'),'-s',args.serial]
work='/home/goblin/goblin-recovery-'+uuid.uuid4().hex[:12]

def shell(*args):
    return subprocess.check_output([*adb,'shell',*args],timeout=30)

def ready():
    end=time.monotonic()+180
    while time.monotonic()<end:
        try:
            result=request(adb,'exec','test -f /var/lib/goblin/uml-imported',timeout=10)
            if result.returncode==0: return
        except (subprocess.SubprocessError,OSError): pass
        time.sleep(.5)
    raise AssertionError('Linux did not become ready')

shell('am','start','-n',PACKAGE+'/.TerminalActivity'); ready()
script='''import os,time,signal
root=ROOT
os.mkdir(root)
with open(root+'/durable','w') as f:
    f.write('synced Linux data\\n'); f.flush(); os.fsync(f.fileno())
fd=os.open(root,os.O_RDONLY); os.fsync(fd); os.close(fd)
os.sync()
signal.signal(signal.SIGHUP,signal.SIG_IGN)
if os.fork(): os._exit(0)
os.setsid()
fd=os.open('/dev/null',os.O_RDWR)
for n in range(3): os.dup2(fd,n)
os.close(fd)
for n in range(1000000):
    with open(root+'/pending','w') as f: f.write(str(n)+'\\n')
    os.replace(root+'/pending',root+'/current')
    time.sleep(.01)
'''.replace('ROOT',repr(work))
r=request(adb,'exec','python3 -c '+shlex.quote(script)); assert r.returncode==0,r.stderr
end=time.monotonic()+20
while time.monotonic()<end:
    r=request(adb,'read',work+'/current')
    if r.returncode==0 and r.stdout.strip().isdigit(): break
    time.sleep(.1)
else: raise AssertionError('Dirty-write fixture did not start')
shell('am','force-stop',PACKAGE)
shell('am','start','-n',PACKAGE+'/.TerminalActivity'); ready()
r=request(adb,'read',work+'/durable'); assert r.returncode==0 and r.stdout==b'synced Linux data\n',r
r=request(adb,'exec','dpkg --audit'); assert r.returncode==0 and not r.stdout.strip(),r.stdout
r=request(adb,'exec','rm -rf '+shlex.quote(work)+'; sync'); assert r.returncode==0
print('PASS: synced user data and installed packages survive Android force-stop and ext4 recovery')
print('GOBLIN PASS')
