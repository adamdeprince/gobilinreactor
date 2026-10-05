#!/usr/bin/env python3
"""Install an APK in place and verify guest data plus the APK-owned runtime.

Requires a running debug UML guest with Python 3. Normally creates only uniquely
named probes. An optional emulator-only check temporarily edits managed config.
Bundled Debian updates require an explicit flag and may only update the pinned
packages or add dependencies, while existing accounts and user config survive.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import time
import uuid
import zipfile

from uml_control import PACKAGE, request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('serial')
    parser.add_argument('apk', type=Path)
    parser.add_argument('--expect-kernel-update', action='store_true')
    parser.add_argument('--allow-bundled-package-updates', action='store_true')
    parser.add_argument('--exercise-managed-config', action='store_true',
                        help='Emulator only: temporarily edit/delete managed config, then restore it')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.exercise_managed_config and not args.serial.startswith('emulator-'):
        parser.error('--exercise-managed-config requires a disposable emulator')
    adb = [os.environ.get('ADB', 'adb'), '-s', args.serial]

    def shell(*command):
        return subprocess.check_output([*adb, 'shell', shlex.join(command)], timeout=30).decode().strip()

    def execute(command, timeout=30):
        result = request(adb, 'exec', command, timeout)
        if result.returncode:
            raise RuntimeError(result.stdout.decode(errors='replace') + result.stderr.decode(errors='replace'))
        return result.stdout.decode()

    def ready():
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            try:
                execute('test -f /var/lib/goblin/uml-imported', 10)
                return
            except (RuntimeError, subprocess.SubprocessError, OSError):
                time.sleep(.5)
        raise RuntimeError('UML did not become ready after the APK update')

    def kernel_hash():
        control = shell('run-as', PACKAGE, 'cat', 'files/uml/ctl-path')
        kernel = str(Path(control).with_name('libgoblinuml-kernel.so'))
        return shell('run-as', PACKAGE, 'sha256sum', kernel).split()[0]

    with zipfile.ZipFile(args.apk) as apk:
        expected_kernel = hashlib.sha256(apk.read('lib/arm64-v8a/libgoblinuml-kernel.so')).hexdigest()
        expected_initrd = hashlib.sha256(apk.read('assets/uml-initramfs.cpio.gz')).hexdigest()
        expected_init = json.loads(apk.read('assets/uml-initramfs-manifest.json'))['files']['init']
        expected_deployment = apk.read('assets/deployment/version').decode().strip()
        bundled = {line.split()[0]: line.split()[1] for line in apk.read('assets/deployment/manifest').decode().splitlines()}
    shell('am', 'start', '-n', PACKAGE + '/.TerminalActivity')
    ready()
    token = uuid.uuid4().hex
    home = '/home/goblin/.goblin-apk-check-' + token
    config = '/etc/goblin-apk-check-' + token + '.conf'
    managed = ['/etc/sudoers.d/90-goblin', '/etc/apt/sources.list.d/goblinreactor.sources',
               '/etc/apt/keyrings/goblinreactor-archive-keyring.gpg', '/etc/resolv.conf']
    paths = [home + '/document', config, '/etc/passwd', '/etc/group', '/etc/shadow',
             '/etc/hostname', '/etc/hosts', '/etc/resolv.conf', '/etc/sudoers',
             '/etc/sudoers.d/90-goblin', '/etc/apt/sources.list.d/debian.sources',
             '/etc/apt/sources.list.d/goblinreactor.sources',
             '/etc/apt/keyrings/goblinreactor-archive-keyring.gpg',
             '/home/goblin/.bashrc', '/home/goblin/.profile']
    snapshot = '''import hashlib,json,os,pathlib,stat,subprocess
paths = PATHS
files = {}
for name in paths:
    p = pathlib.Path(name)
    try: s = p.lstat()
    except FileNotFoundError: files[name] = None; continue
    item = [s.st_mode,s.st_uid,s.st_gid]
    if stat.S_ISREG(s.st_mode): item.append(hashlib.sha256(p.read_bytes()).hexdigest())
    elif stat.S_ISLNK(s.st_mode): item.append(os.readlink(p))
    files[name] = item
print(json.dumps(dict(files=files,
    accounts={p:pathlib.Path(p).read_text().splitlines() for p in ['/etc/passwd','/etc/group','/etc/shadow']},
    packages=subprocess.check_output(['dpkg-query','-W','-f=${Package} ${Version} ${db:Status-Status}\\n'],text=True),
    kernel=os.uname().version,
    boot_id=pathlib.Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
    init_name=pathlib.Path('/proc/1/comm').read_text().strip(),
    init_sha256=hashlib.sha256(pathlib.Path('/run/goblin/agent' if pathlib.Path('/run/goblin/agent').exists() else '/proc/1/exe').read_bytes()).hexdigest())))
'''.replace('PATHS', repr(paths))

    def state():
        return json.loads(execute('python3 -c ' + shlex.quote(snapshot)))

    try:
        create = '''import os,pathlib,pwd
home=pathlib.Path(HOME_PATH); home.mkdir(mode=0o700)
document=home/'document'; document.write_text('user data survives Android package replacement\\n'); document.chmod(0o600)
user=pwd.getpwnam('goblin')
for p in [home,document]: os.chown(p,user.pw_uid,user.pw_gid)
pathlib.Path(CONFIG_PATH).write_text('user-owned Linux configuration\\n')
os.sync()
'''.replace('HOME_PATH', repr(home)).replace('CONFIG_PATH', repr(config))
        execute('python3 -c ' + shlex.quote(create))
        if args.exercise_managed_config:
            edit = '''import os,pathlib,shutil,stat
paths=MANAGED_PATHS
backup=pathlib.Path(BACKUP_PATH); backup.mkdir(mode=0o700)
for name in paths:
    p=pathlib.Path(name); s=p.lstat()
    assert stat.S_ISREG(s.st_mode), 'Fixture expects regular files: '+name
    saved=backup/p.name; shutil.copy2(p,saved); os.chown(saved,s.st_uid,s.st_gid)
for name in paths[:2]:
    with open(name,'ab') as f: f.write(b'\\n# Administrator edit: retain across Android updates\\n')
os.chmod(paths[0],0o400)
os.unlink(paths[2])
pathlib.Path(paths[3]).write_bytes(b'')
os.sync()
'''.replace('MANAGED_PATHS', repr(managed)).replace('BACKUP_PATH', repr(home + '/config-backup'))
            execute('python3 -c ' + shlex.quote(edit))
        before = state()
        old_kernel_hash = kernel_hash()
        disk_before = shell('run-as', PACKAGE, 'stat', '-c', '%i', 'files/uml/rootfs.ext4')
        print('Installing APK in place; guest probe data and configuration recorded.', flush=True)
        subprocess.run([*adb, 'install', '-r', str(args.apk.resolve())], check=True, timeout=180)
        shell('am', 'start', '-n', PACKAGE + '/.TerminalActivity')
        ready()
        after = state()
        changed = [p for p in paths if before['files'][p] != after['files'][p]]
        if args.allow_bundled_package_updates:
            for name in before['accounts']:
                previous = {line.split(':')[0]:line for line in before['accounts'][name]}
                current = {line.split(':')[0]:line for line in after['accounts'][name]}
                assert all(current.get(k) == v for k,v in previous.items()), 'Existing account changed: ' + name
                if name in changed: changed.remove(name)
        assert not changed, 'Guest files changed: ' + ', '.join(changed)
        if args.allow_bundled_package_updates:
            current = {row.split()[0]:row.split()[1:] for row in after['packages'].splitlines()}
            for row in before['packages'].splitlines():
                package, version, status = row.split()
                assert current.get(package) in ([version,status], [bundled.get(package),'installed']), 'Unrelated package changed: ' + package
        else: assert before['packages'] == after['packages'], 'Guest package set changed'
        assert disk_before == shell('run-as', PACKAGE, 'stat', '-c', '%i', 'files/uml/rootfs.ext4'), 'Guest disk was replaced'
        assert before['boot_id'] != after['boot_id'], 'Guest did not restart'
        assert kernel_hash() == expected_kernel, 'Installed kernel differs from new APK'
        assert shell('run-as', PACKAGE, 'sha256sum', 'files/uml/initramfs.cpio.gz').split()[0] == expected_initrd
        assert after['init_name'] == 'systemd', 'Debian service manager is not PID 1'
        assert after['init_sha256'] == expected_init, 'Running terminal broker differs from new APK initramfs'
        assert execute('cat /var/lib/goblin/deployment-complete').strip() == expected_deployment
        if args.expect_kernel_update:
            assert old_kernel_hash != expected_kernel, 'Test APK does not contain a changed kernel'
            assert before['kernel'] != after['kernel'], 'Guest is still reporting the previous kernel build'
        execute('set -e; su -s /bin/sh goblin -c "sudo -n true"; test -z "$(dpkg --audit)"; sync')
        report = dict(result='PASS', serial=args.serial,
                      apk_sha256=hashlib.sha256(args.apk.read_bytes()).hexdigest(),
                      kernel_before=before['kernel'], kernel_after=after['kernel'],
                      kernel_sha256=expected_kernel, initramfs_sha256=expected_initrd,
                      running_init_sha256=expected_init, deployment=expected_deployment,
                      preserved_paths=len(paths), preserved_packages=len(before['packages'].splitlines()),
                      same_disk=True, edited_and_deleted_config_tested=args.exercise_managed_config)
        print(json.dumps(report, indent=2))
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2) + '\n')
    finally:
        try:
            cleanup = '''import os,pathlib,shutil
home=pathlib.Path(HOME_PATH)
for name in MANAGED_PATHS:
    saved=home/'config-backup'/pathlib.Path(name).name
    if saved.exists() or saved.is_symlink(): os.replace(saved,name)
if home.exists(): shutil.rmtree(home)
pathlib.Path(CONFIG_PATH).unlink(missing_ok=True)
os.sync()
'''.replace('HOME_PATH', repr(home)).replace('MANAGED_PATHS', repr(managed)).replace('CONFIG_PATH', repr(config))
            execute('python3 -c ' + shlex.quote(cleanup))
        except (RuntimeError, subprocess.SubprocessError, OSError) as error:
            print('Could not remove upgrade probes:', home, config, error)


if __name__ == '__main__':
    main()
