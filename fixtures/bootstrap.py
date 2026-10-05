#!/usr/bin/env python3
"""Prepare the foreign-stage Debian seed; dpkg installs it inside Goblin.

The seed contains complete SHA256-pinned package payloads and their archives,
but an EMPTY dpkg database. No package or maintainer script runs on the host.
"""
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import stat
import struct
import subprocess
import tarfile
from concurrent.futures import ThreadPoolExecutor

HERE = Path(__file__).resolve().parent
OUT = HERE / "build"
OUT.mkdir(exist_ok=True)
PACKAGES = json.loads((HERE / "debian-bootstrap.lock.json").read_text())


def download(package):
    path = OUT / Path(package["Filename"]).name
    if not path.exists():
        temporary = path.with_suffix(".download")
        subprocess.run(["curl", "-fsSL", "--retry", "2", "--max-time", "120",
                        "https://deb.debian.org/debian/" + package["Filename"],
                        "-o", str(temporary)], check=True)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != package["SHA256"]:
            raise RuntimeError(f"SHA256 mismatch: {temporary}")
        temporary.replace(path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != package["SHA256"]:
        raise RuntimeError(f"SHA256 mismatch: {path}")
    return path


def build():
    with ThreadPoolExecutor(max_workers=4) as pool:
        archives = list(pool.map(download, PACKAGES))
    entries = {}
    hardlinks = []
    for package, path in zip(PACKAGES, archives):
        entries["var/cache/goblin-bootstrap/" + path.name] = (stat.S_IFREG | 0o644, path.read_bytes())
        members = subprocess.check_output(["ar", "t", str(path)]).decode().splitlines()
        member = next(n for n in members if n.startswith("data.tar."))
        data = subprocess.check_output(["ar", "p", str(path), member])
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as tar:
            for item in tar:
                name = str(PurePosixPath(item.name))
                if name == ".":
                    continue
                if name.startswith("/") or ".." in PurePosixPath(name).parts:
                    raise RuntimeError(f"unsafe package path: {name}")
                if item.isfile():
                    entries[name] = (stat.S_IFREG | (item.mode & 0o7777), tar.extractfile(item).read())
                elif item.isdir():
                    entries[name] = (stat.S_IFDIR | (item.mode & 0o7777), b"")
                elif item.issym():
                    entries[name] = (stat.S_IFLNK | 0o777, item.linkname.encode())
                elif item.islnk():
                    hardlinks.append((name, str(PurePosixPath(item.linkname))))
                else:
                    raise RuntimeError(f"unsupported seed entry: {name}")
    # dpkg replaces these initial copies with VFS hard links during unpacking.
    for name, target in hardlinks:
        entries[name] = entries[target]
    for name, target in (("bin", "usr/bin"), ("sbin", "usr/sbin"), ("lib", "usr/lib"), ("usr/bin/sh", "dash")):
        entries[name] = (stat.S_IFLNK | 0o777, target.encode())
    for name in ("dev", "dev/pts", "proc", "proc/self", "sys", "run", "root", "tmp",
                 "var/lib/dpkg/info", "var/lib/dpkg/updates", "var/lib/dpkg/triggers",
                 "var/lib/apt/lists/partial", "var/cache/apt/archives/partial", "var/log/apt",
                 "etc/apt/apt.conf.d", "etc/apt/sources.list.d"):
        entries.setdefault(name, (stat.S_IFDIR | (0o1777 if name == "tmp" else 0o755), b""))
    files = {
        "var/lib/dpkg/status": "",
        "var/lib/dpkg/available": "",
        "etc/passwd": "root:x:0:0:root:/root:/bin/bash\n",
        "etc/group": "root:x:0:\n",
        "etc/hostname": "goblin\n",
        "etc/nsswitch.conf": "passwd: files\ngroup: files\nshadow: files\nhosts: files dns\n",
        "etc/hosts": "127.0.0.1 localhost goblin\n::1 localhost\n",
        "etc/resolv.conf": "nameserver 127.0.0.53\noptions timeout:5 attempts:2\n",
        "etc/apt/apt.conf.d/99-goblin": 'APT::Sandbox::User "root";\nAcquire::Languages "none";\nAPT::Install-Recommends "false";\nAcquire::Retries "2";\n',
        "etc/apt/sources.list.d/debian.sources": "Types: deb\nURIs: https://deb.debian.org/debian\nSuites: trixie\nComponents: main\nArchitectures: arm64\nSigned-By: /usr/share/keyrings/debian-archive-keyring.gpg\n",
        "usr/sbin/policy-rc.d": "#!/bin/sh\n# This userspace session has no system service manager.\nexit 101\n",
        "usr/local/sbin/goblin-bootstrap": """#!/bin/sh
set -eu
export DEBIAN_FRONTEND=noninteractive
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
mkdir -p /var/log /var/lib/goblin
if test -f /var/lib/goblin/bootstrap-complete; then exit 0; fi
# Populate Debian's standard system accounts before configuring packages.
update-passwd
# Seed extraction is the foreign first stage. All package scripts below run
# under the guest kernel. Dependency cycles are resolved by configuration.
set --
while read -r package version archive; do
    state=$(dpkg-query -W -f='${db:Status-Status} ${Version}' "$package" 2>/dev/null || true)
    case "$state" in
        "unpacked $version"|"installed $version"|"triggers-pending $version"|"triggers-awaited $version") ;;
        *) set -- "$@" "/var/cache/goblin-bootstrap/$archive" ;;
    esac
done < /var/cache/goblin-bootstrap/manifest
if test "$#" -gt 0; then dpkg --force-depends --force-confold --unpack "$@"; fi
dpkg --force-confold --configure -a
test -z "$(dpkg --audit)"
printf 'Debian bootstrap complete\\n' > /var/lib/goblin/bootstrap-complete
sync
""",
    }
    files["etc/passwd"] = entries["usr/share/base-passwd/passwd.master"][1].decode()
    files["etc/group"] = entries["usr/share/base-passwd/group.master"][1].decode()
    files["var/cache/goblin-bootstrap/manifest"] = "".join(p["Package"] + " " + p["Version"] + " " + Path(p["Filename"]).name + "\n" for p in PACKAGES)
    (OUT / "goblin-bootstrap").write_text(files["usr/local/sbin/goblin-bootstrap"])
    (OUT / "bootstrap-manifest").write_text(files["var/cache/goblin-bootstrap/manifest"])
    for name, content in files.items():
        entries[name] = (stat.S_IFREG | (0o755 if name in ("usr/sbin/policy-rc.d", "usr/local/sbin/goblin-bootstrap") else 0o644), content.encode())
    for name in list(entries):
        for parent in PurePosixPath(name).parents:
            if str(parent) != ".":
                entries.setdefault(str(parent), (stat.S_IFDIR | 0o755, b""))
    ordered = sorted(entries, key=lambda n: (stat.S_ISLNK(entries[n][0]), n.count("/"), n))
    with (OUT / "debian.pack").open("wb") as output:
        output.write(b"GOBLINFS" + struct.pack("<I", len(ordered)))
        for name in ordered:
            mode, data = entries[name]
            encoded = name.encode()
            output.write(struct.pack("<IIQ", len(encoded), mode, len(data)))
            output.write(encoded)
            output.write(data)
    print(f"Built {len(PACKAGES)}-package foreign seed, {len(entries)} entries; dpkg state is empty.")


if __name__ == "__main__":
    build()
