#!/usr/bin/env python3
"""Run the real deployment script in disposable chroots in the Linux builder.

No deployment command runs against the builder's own /etc or package database.
"""
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'fixtures/deploy-guest.sh'
STAGE = 'var/lib/goblin/deployment'
DEFAULTS = 'var/lib/goblin/config-defaults'
COMPLETE = 'var/lib/goblin/deployment-complete'
SUDO = 'etc/sudoers.d/90-goblin'
SOURCE = 'etc/apt/sources.list.d/goblinreactor.sources'
KEY = 'etc/apt/keyrings/goblinreactor-archive-keyring.gpg'
DNS = 'etc/resolv.conf'
SOURCE_V1 = b'Types: deb\nURIs: https://apt.example.test\nSuites: stable\nComponents: main\n'
SOURCE_V2 = SOURCE_V1.replace(b'stable', b'next')


class DeploymentUpgradeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.uname().sysname != 'Linux' or os.geteuid() != 0:
            raise RuntimeError('Run as root inside the disposable Linux builder')
        # Debian mounts /tmp nodev; the chroot needs a working /dev/null.
        cls.workspace = tempfile.TemporaryDirectory(prefix='goblin-deployment-test-', dir='/var/tmp')
        cls.base = Path(cls.workspace.name) / 'base'
        cls.base.mkdir()
        # Real Debian tools and their shared libraries; no mock install/cmp/mv.
        for program in ['sh', 'cat', 'mkdir', 'chmod', 'install', 'stat', 'cmp',
                        'dirname', 'mktemp', 'mv', 'rm', 'sync', 'visudo']:
            path = shutil.which(program)
            if path is None:
                raise RuntimeError('Linux builder needs ' + program)
            cls.copy_tool(path, '/bin/sh' if program == 'sh' else path)

    @classmethod
    def copy_tool(cls, source, target):
        paths = [(source, target)]
        libraries = subprocess.check_output(['ldd', source], text=True)
        paths += [(p, p) for p in re.findall(r'(/\S+)\s+\(', libraries)]
        for source, target in paths:
            destination = cls.base / target.lstrip('/')
            if not destination.exists():
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, destination)
                destination.chmod(0o755)

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(dir=self.workspace.name, prefix='case-'))
        shutil.copytree(self.base, self.root, dirs_exist_ok=True)
        (self.root / 'dev').mkdir()
        os.mknod(self.root / 'dev/null', stat.S_IFCHR | 0o666, os.makedev(1, 3))
        self.write('etc/passwd', b'root:x:0:0:root:/root:/bin/sh\n')
        self.write('etc/group', b'root:x:0:\n')
        self.write('etc/sudoers', b'root ALL=(ALL:ALL) ALL\n@includedir /etc/sudoers.d\n')
        self.write(STAGE + '/configure.sh', SCRIPT.read_bytes())
        self.write(STAGE + '/manifest', b'')
        self.stage('v1', SOURCE_V1, b'key-v1')

    def write(self, name, data, mode=0o644):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)
        return path

    def stage(self, version, source=SOURCE_V2, key=b'key-v2'):
        self.write(STAGE + '/version', version.encode() + b'\n')
        self.write(STAGE + '/goblinreactor.sources', source)
        self.write(STAGE + '/goblinreactor-archive-keyring.gpg', key)

    def deploy(self):
        result = subprocess.run(['chroot', str(self.root), '/bin/sh', '/' + STAGE + '/configure.sh'],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.root / COMPLETE).read_bytes(), (self.root / STAGE / 'version').read_bytes())

    def test_fresh_install_and_unchanged_defaults_upgrade(self):
        self.deploy()
        self.assertIn(b'NOPASSWD: ALL', (self.root / SUDO).read_bytes())
        self.assertEqual(stat.S_IMODE((self.root / SUDO).stat().st_mode), 0o440)
        self.assertEqual((self.root / SOURCE).read_bytes(), SOURCE_V1)
        self.assertEqual((self.root / KEY).read_bytes(), b'key-v1')
        self.assertEqual((self.root / DNS).read_bytes(), b'nameserver 10.0.2.3\n')
        self.stage('v2')
        self.deploy()
        self.assertEqual((self.root / SOURCE).read_bytes(), SOURCE_V2)
        self.assertEqual((self.root / KEY).read_bytes(), b'key-v2')

    def test_edits_and_unrelated_user_data_survive(self):
        self.deploy()
        edits = {SUDO: b'# Sudo disabled by the administrator\n', SOURCE: b'# Repository disabled\n',
                 KEY: b'custom-key', DNS: b'nameserver 192.0.2.53\n',
                 'home/goblin/work.txt': b'user documents\n', 'etc/hostname': b'my-phone\n',
                 'var/lib/dpkg/status': b'Package: user-installed-package\nStatus: install ok installed\n'}
        for path, data in edits.items():
            self.write(path, data)
        # Invalid user sudo config must not stop boot or be silently repaired.
        self.write('etc/sudoers', b'user is still editing this file\n')
        self.stage('v2')
        self.deploy()
        for path, data in edits.items():
            self.assertEqual((self.root / path).read_bytes(), data, path)
        self.assertEqual((self.root / DEFAULTS / SOURCE).read_bytes(), SOURCE_V2)
        self.assertEqual((self.root / DEFAULTS / KEY).read_bytes(), b'key-v2')

    def test_deleted_files_stay_deleted_across_updates(self):
        self.deploy()
        for path in [SUDO, SOURCE, KEY, DNS]:
            (self.root / path).unlink()
        for version in ['v2', 'v3']:
            self.stage(version)
            self.deploy()
            for path in [SUDO, SOURCE, KEY, DNS]:
                self.assertFalse((self.root / path).exists(), path)

    def test_empty_dns_is_a_user_setting(self):
        self.deploy()
        self.write(DNS, b'')
        self.stage('v2')
        self.deploy()
        self.assertEqual((self.root / DNS).read_bytes(), b'')

    def test_symlinks_including_dangling_links_are_preserved(self):
        self.deploy()
        self.write('etc/custom.sources', SOURCE_V1)
        for path, target in [(SOURCE, '/etc/custom.sources'), (DNS, '/run/user-dns.conf')]:
            (self.root / path).unlink()
            (self.root / path).symlink_to(target)
        self.stage('v2')
        self.deploy()
        self.assertEqual(os.readlink(self.root / SOURCE), '/etc/custom.sources')
        self.assertEqual((self.root / 'etc/custom.sources').read_bytes(), SOURCE_V1)
        self.assertEqual(os.readlink(self.root / DNS), '/run/user-dns.conf')

    def test_permissions_ownership_and_hard_links_are_preserved(self):
        self.deploy()
        (self.root / SOURCE).chmod(0o600)
        os.chown(self.root / KEY, 1000, 1001)
        os.link(self.root / SUDO, self.root / 'etc/my-sudo-rule')
        self.stage('v2')
        self.deploy()
        source = (self.root / SOURCE).stat()
        key = (self.root / KEY).stat()
        self.assertEqual(stat.S_IMODE(source.st_mode), 0o600)
        self.assertEqual((self.root / SOURCE).read_bytes(), SOURCE_V1)
        self.assertEqual((key.st_uid, key.st_gid), (1000, 1001))
        self.assertEqual((self.root / KEY).read_bytes(), b'key-v1')
        self.assertEqual((self.root / SUDO).stat().st_nlink, 2)

    def test_directory_in_place_of_config_is_preserved(self):
        self.deploy()
        (self.root / SOURCE).unlink()
        self.write(SOURCE + '/user-file', b'keep me')
        self.stage('v2')
        self.deploy()
        self.assertEqual((self.root / SOURCE / 'user-file').read_bytes(), b'keep me')

    def test_legacy_deployment_retains_edits_and_deletions(self):
        self.write(COMPLETE, b'old-apk-without-config-snapshots\n')
        self.write(SOURCE, b'# custom legacy configuration\n')
        for version in ['v2', 'v3']:
            self.stage(version)
            self.deploy()
            self.assertEqual((self.root / SOURCE).read_bytes(), b'# custom legacy configuration\n')
            for path in [SUDO, KEY, DNS]:
                self.assertFalse((self.root / path).exists(), path)

    def test_legacy_matching_defaults_can_receive_future_updates(self):
        self.write(COMPLETE, b'old-apk-without-config-snapshots\n')
        original = self.write(SOURCE, SOURCE_V1).stat()
        self.deploy()
        self.assertEqual((self.root / SOURCE).stat().st_ino, original.st_ino)
        self.stage('v2')
        self.deploy()
        self.assertEqual((self.root / SOURCE).read_bytes(), SOURCE_V2)

    def test_retry_after_live_file_was_updated_before_snapshot(self):
        self.deploy()
        self.stage('v2')
        updated = self.write(SOURCE, SOURCE_V2).stat()
        self.deploy()
        self.assertEqual((self.root / SOURCE).stat().st_ino, updated.st_ino)
        source_v3 = SOURCE_V2.replace(b'next', b'third')
        self.stage('v3', source_v3)
        self.deploy()
        self.assertEqual((self.root / SOURCE).read_bytes(), source_v3)

    def test_repeated_startup_leaves_files_and_markers_untouched(self):
        self.deploy()
        paths = [SUDO, SOURCE, KEY, DNS, COMPLETE, DEFAULTS + '/' + SOURCE]
        before = {p: (self.root / p).stat() for p in paths}
        self.deploy()
        for path, old in before.items():
            current = (self.root / path).stat()
            self.assertEqual((current.st_ino, current.st_mtime_ns), (old.st_ino, old.st_mtime_ns), path)


if __name__ == '__main__':
    unittest.main(verbosity=2)
