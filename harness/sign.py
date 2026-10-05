#!/usr/bin/env python3
"""Sign local artifacts; retain release keys outside disposable build output."""
import argparse
import os
from pathlib import Path
import secrets
import subprocess

ROOT = Path(__file__).resolve().parent.parent

def run(*args):
    subprocess.run([str(a) for a in args], check=True)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('variant', choices=['debug', 'release'])
    p.add_argument('input', type=Path); p.add_argument('output', type=Path)
    p.add_argument('--build-tools', type=Path, required=True)
    args = p.parse_args()
    signer = args.build_tools / 'apksigner'
    old = ROOT / 'probe/debug.keystore'
    if args.variant == 'debug':
        if not old.exists():
            run('keytool', '-genkeypair', '-keystore', old, '-storepass', 'android', '-keypass', 'android',
                '-alias', 'goblindebug', '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000',
                '-dname', 'CN=goblin-linux debug, OU=probe, O=goblin-linux, C=US')
        options = ['--ks', old, '--ks-pass', 'pass:android', '--ks-key-alias', 'goblindebug']
    else:
        directory = Path(os.environ.get('GOBLIN_SIGNING_DIR', ROOT / 'harness/signing'))
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        key, password, lineage = (directory / name for name in ['release.keystore', 'password', 'lineage'])
        if not key.exists():
            if password.exists() or lineage.exists():
                raise SystemExit('Incomplete signing directory: recover the existing key; do not silently replace it')
            fd = os.open(password, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'w') as stream: stream.write(secrets.token_urlsafe(48) + '\n')
            run('keytool', '-genkeypair', '-keystore', key, '-storepass:file', password,
                '-alias', 'goblinrelease', '-keyalg', 'RSA', '-keysize', '4096', '-validity', '10000',
                '-dname', 'CN=Goblin Linux, O=Goblin Linux, C=US')
            key.chmod(0o600)
        if not password.exists(): raise SystemExit('Missing release keystore password file')
        if not lineage.exists() and old.exists():
            run(signer, 'rotate', '--out', lineage, '--old-signer', '--ks', old,
                '--ks-pass', 'pass:android', '--ks-key-alias', 'goblindebug',
                '--set-installed-data', 'true', '--set-rollback', 'false',
                '--new-signer', '--ks', key, '--ks-pass', 'file:' + str(password), '--ks-key-alias', 'goblinrelease')
        certificate = directory / 'release-certificate.pem'
        if not certificate.exists():
            run('keytool', '-exportcert', '-rfc', '-keystore', key, '-storepass:file', password,
                '-alias', 'goblinrelease', '-file', certificate)
        options = ['--ks', key, '--ks-pass', 'file:' + str(password), '--ks-key-alias', 'goblinrelease',
                   '--v1-signing-enabled', 'false', '--v2-signing-enabled', 'false',
                   '--v3-signing-enabled', 'true', '--rotation-min-sdk-version', '28']
        if lineage.exists(): options += ['--lineage', lineage]
    run(signer, 'sign', *options, '--out', args.output, args.input)
    run(signer, 'verify', '--verbose', '--print-certs', args.output)

if __name__ == '__main__': main()
