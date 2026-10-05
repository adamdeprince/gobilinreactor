#!/usr/bin/env python3
"""Build a signed Play App Bundle and test APK set from the release APK.

Run VARIANT=release bash harness/build.sh first. --fetch-tool downloads Google's
pinned bundletool once; ordinary builds use the verified local copy offline.
Native executables require Android's native-library extraction, so that setting
is retained in generated APKs. Nothing is uploaded to Google Play.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = '1.18.3'
SHA256 = 'a099cfa1543f55593bc2ed16a70a7c67fe54b1747bb7301f37fdfd6d91028e29'
URL = f'https://github.com/google/bundletool/releases/download/{VERSION}/bundletool-all-{VERSION}.jar'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apk', type=Path, default=ROOT / 'harness/build-release/goblin-sentry.apk')
    parser.add_argument('--output', type=Path, default=ROOT / 'harness/build-bundle')
    parser.add_argument('--fetch-tool', action='store_true')
    args = parser.parse_args()
    tool = Path(os.environ.get('BUNDLETOOL', ROOT / f'harness/tools/bundletool-{VERSION}.jar'))
    if not tool.exists() and args.fetch_tool:
        tool.parent.mkdir(parents=True, exist_ok=True)
        temporary = tool.with_suffix('.download')
        try:
            with urllib.request.urlopen(URL) as source, temporary.open('wb') as destination:
                shutil.copyfileobj(source, destination)
            if hashlib.sha256(temporary.read_bytes()).hexdigest() != SHA256:
                raise SystemExit('bundletool checksum mismatch')
            temporary.replace(tool)
        finally:
            temporary.unlink(missing_ok=True)
    if not tool.exists():
        raise SystemExit('Run with --fetch-tool once, or set BUNDLETOOL to the pinned JAR')
    if hashlib.sha256(tool.read_bytes()).hexdigest() != SHA256:
        raise SystemExit('bundletool checksum mismatch')
    sdk = Path(os.environ.get('ANDROID_SDK_ROOT', os.environ.get('ANDROID_HOME', Path.home() / 'Library/Android/sdk')))
    bt = sdk / 'build-tools' / os.environ.get('BT_VER', '36.1.0')
    jdk = Path(os.environ.get('JAVA_HOME', '/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home'))
    java, jarsigner = jdk / 'bin/java', jdk / 'bin/jarsigner'
    signing = Path(os.environ.get('GOBLIN_SIGNING_DIR', ROOT / 'harness/signing'))
    key, password = signing / 'release.keystore', signing / 'password'
    if not key.exists() or not password.exists():
        raise SystemExit('Build the signed release APK first; an existing release key is required')
    env = dict(os.environ, JAVA_HOME=str(jdk))
    def run(*command):
        return subprocess.check_output([str(c) for c in command], env=env, text=True, stderr=subprocess.STDOUT)
    signature = run(bt / 'apksigner', 'verify', '--verbose', '--print-certs', args.apk)
    if 'CN=Goblin Linux' not in signature:
        raise SystemExit('The source must be a release-signed Goblin APK')
    out = args.output; out.mkdir(parents=True, exist_ok=True)
    proto = out / 'proto.apk'
    run(bt / 'aapt2', 'convert', '--output-format', 'proto', '-o', proto, args.apk)
    module = out / 'base.zip'
    with zipfile.ZipFile(proto) as source, zipfile.ZipFile(module, 'w', compression=zipfile.ZIP_DEFLATED) as target:
        for name in source.namelist():
            if name == 'AndroidManifest.xml': destination = 'manifest/AndroidManifest.xml'
            elif name.startswith('classes') and name.endswith('.dex'): destination = 'dex/' + name
            elif name == 'resources.pb' or name.startswith(('assets/', 'lib/', 'res/')): destination = name
            else: continue
            if not name.endswith('/'): target.writestr(destination, source.read(name))
    config = out / 'BundleConfig.json'
    config.write_text(json.dumps({'optimizations': {
        'uncompressNativeLibraries': {'enabled': False, 'alignment': 'PAGE_ALIGNMENT_16K'},
        'uncompressDexFiles': {'enabled': False},
    }}, indent=2) + '\n')
    bundle = out / 'goblin.aab'
    print('Building App Bundle', flush=True)
    print(run(java, '-jar', tool, 'build-bundle', f'--modules={module}', f'--output={bundle}', f'--config={config}', '--overwrite'))
    print(run(jarsigner, '-keystore', key, '-storepass:file', password, '-digestalg', 'SHA-256', '-sigalg', 'SHA256withRSA', bundle, 'goblinrelease'))
    verified = run(jarsigner, '-verify', bundle)
    if 'jar verified.' not in verified: raise SystemExit(verified)
    run(java, '-jar', tool, 'validate', f'--bundle={bundle}')
    print('Bundle validation passed', flush=True)
    apks = out / 'goblin.apks'
    options = [f'--ks={key}', f'--ks-pass=file:{password}', '--ks-key-alias=goblinrelease']
    print('Generating device APKs for validation', flush=True)
    with tempfile.TemporaryDirectory(prefix='goblin-bundle-signing-') as temporary:
        lineage = signing / 'lineage'
        if lineage.exists():
            # sign.py's lineage starts at the historical development key.
            # Bundletool requires the oldest signer even above the rotation API.
            oldest_key = ROOT / 'probe/debug.keystore'
            if not oldest_key.exists():
                raise SystemExit('The existing signing lineage requires probe/debug.keystore for local APK-set generation')
            oldest = Path(temporary) / 'oldest.properties'
            oldest.write_text(f'ks={oldest_key}\nks-key-alias=goblindebug\nks-pass=pass:android\n')
            oldest.chmod(0o600)
            options += [f'--lineage={lineage}', '--rotation-min-sdk-version=28', f'--oldest-signer={oldest}']
        print(run(java, '-jar', tool, 'build-apks', f'--bundle={bundle}', f'--output={apks}', f'--aapt2={bt / "aapt2"}', '--overwrite', *options))
    report = {'bundletool_version': VERSION, 'bundletool_sha256': SHA256,
              'source_apk_sha256': hashlib.sha256(args.apk.read_bytes()).hexdigest(),
              'aab_sha256': hashlib.sha256(bundle.read_bytes()).hexdigest(),
              'apks_sha256': hashlib.sha256(apks.read_bytes()).hexdigest(),
              'bundle_validated': True, 'device_tests': 'Run install-apks and the release acceptance suite separately'}
    (out / 'build.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    print(f'App Bundle: {bundle}\nLocal test APK set: {apks}')


if __name__ == '__main__':
    try: main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.output)
