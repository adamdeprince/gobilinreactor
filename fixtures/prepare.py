#!/usr/bin/env python3
"""Build a small Debian ARM64 test rootfs without running package code.

Downloads are pinned by SHA256. Only regular files, directories and symlinks
are included; maintainer scripts are never run. The result is a harness fixture,
not an installed Debian system or a package-management implementation.
"""
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import stat
import struct
import subprocess
import tarfile

HERE = Path(__file__).resolve().parent
OUT = HERE / "build"
OUT.mkdir(exist_ok=True)
entries = {}
packages = json.loads((HERE / "debian-arm64.lock.json").read_text())
sysroot = OUT / "sysroot"
for package in packages:
    path = OUT / Path(package["Filename"]).name
    if not path.exists():
        subprocess.run(["curl", "-fL", "--retry", "2", "--max-time", "120",
                        "https://deb.debian.org/debian/" + package["Filename"],
                        "-o", str(path)], check=True)
    if hashlib.sha256(path.read_bytes()).hexdigest() != package["SHA256"]:
        raise SystemExit(f"SHA256 mismatch: {path}")
    if package.get("Role") == "test-package":
        entries["tmp/hello.deb"] = (stat.S_IFREG | 0o644, path.read_bytes())
        continue
    members = subprocess.check_output(["ar", "t", str(path)]).decode().splitlines()
    data = next(m for m in members if m.startswith("data.tar."))
    payload = subprocess.check_output(["ar", "p", str(path), data])
    with tarfile.open(fileobj=io.BytesIO(payload), mode="r:*") as archive:
        for member in archive:
            name = str(PurePosixPath(member.name))
            if name == ".":
                continue
            if name.startswith("/") or ".." in PurePosixPath(name).parts:
                raise SystemExit(f"unsafe package member: {name}")
            if package["Package"] in ("libc6", "libc6-dev", "linux-libc-dev") and name.startswith(("usr/include/", "usr/lib/", "lib/")):
                target = sysroot / name
                target.parent.mkdir(parents=True, exist_ok=True)
                if member.isfile():
                    target.write_bytes(archive.extractfile(member).read())
                elif member.issym() and not target.exists() and not target.is_symlink():
                    target.symlink_to(member.linkname)
                elif member.isdir():
                    target.mkdir(exist_ok=True)
            if package.get("Role") == "build-only":
                continue
            # Exclude documentation, locales and development-only payloads.
            if not (name.startswith(("usr/bin/", "bin/", "usr/sbin/", "sbin/", "usr/lib/", "lib/", "usr/share/dpkg/", "usr/share/keyrings/", "etc/"))
                    or name in ("usr", "usr/bin", "usr/lib", "usr/sbin", "bin", "lib", "sbin", "etc")):
                continue
            if member.isfile():
                entries[name] = (stat.S_IFREG | (member.mode & 0o777),
                                 archive.extractfile(member).read())
            elif member.issym():
                entries[name] = (stat.S_IFLNK | 0o777, member.linkname.encode())
            elif member.isdir():
                entries[name] = (stat.S_IFDIR | 0o755, b"")
            elif member.islnk():
                target = str(PurePosixPath(member.linkname))
                entries[name] = (stat.S_IFLNK | 0o777, ("/" + target).encode())

# Debian's merged-/usr aliases are normally supplied by base-files.
if not (sysroot / "lib").exists():
    (sysroot / "lib").symlink_to("usr/lib")
for name, target in (("bin", "usr/bin"), ("lib", "usr/lib"), ("sbin", "usr/sbin"),
                     ("usr/bin/sh", "dash")):
    entries[name] = (stat.S_IFLNK | 0o777, target.encode())
for name in ("etc", "tmp", "dev", "dev/pts", "proc", "proc/self", "root"):
    entries[name] = (stat.S_IFDIR | 0o755, b"")
entries["etc/passwd"] = (stat.S_IFREG | 0o644, b"root:x:0:0:root:/root:/bin/bash\n")
entries["etc/group"] = (stat.S_IFREG | 0o644, b"root:x:0:\n")
entries["etc/hostname"] = (stat.S_IFREG | 0o644, b"goblin\n")
entries["etc/os-release"] = (stat.S_IFREG | 0o644,
    b'PRETTY_NAME="Debian GNU/Linux 13 (trixie)"\nID=debian\nVERSION_ID="13"\n')

# A minimal package database describes this deliberately unpacked test image.
# Real package installation is exercised in the guest, using the pinned hello
# package; no maintainer scripts execute on the development host.
status = []
for package in packages:
    if package.get("Role"):
        continue
    status.append("Package: " + package["Package"] + "\nStatus: install ok installed\n"
                  "Architecture: arm64\nVersion: " + package["Version"] +
                  "\nMaintainer: Goblin fixture <fixture@localhost>\nDescription: pinned test runtime\n")
entries["var/lib/dpkg/status"] = (stat.S_IFREG | 0o644, ("\n".join(status) + "\n").encode())
entries["var/lib/dpkg/available"] = (stat.S_IFREG | 0o644, b"")
for name in ("var/lib/dpkg/info", "var/lib/dpkg/updates", "var/lib/dpkg/triggers",
             "var/lib/apt/lists/partial", "var/cache/apt/archives/partial", "var/log/apt",
             "etc/apt/sources.list.d", "etc/apt/apt.conf.d"):
    entries[name] = (stat.S_IFDIR | 0o755, b"")
entries["etc/apt/apt.conf.d/99-goblin-fixture"] = (stat.S_IFREG | 0o644,
    b'APT::Sandbox::User "root";\nAcquire::Languages "none";\n')
entries["etc/apt/sources.list"] = (stat.S_IFREG | 0o644,
    b"deb [trusted=yes] http://127.0.0.1:18080 ./\n")
entries["etc/nsswitch.conf"] = (stat.S_IFREG | 0o644, b"passwd: files\ngroup: files\nhosts: files dns\n")
entries["etc/hosts"] = (stat.S_IFREG | 0o644, b"127.0.0.1 localhost\n::1 localhost\n")
entries["etc/resolv.conf"] = (stat.S_IFREG | 0o644, b"nameserver 1.1.1.1\n")

# Materialize the pinned index for the APK's loopback-only HTTP fixture server.
# Trust applies only to that local test source; packages are pinned and verified
# above before the repository is built.
repository = OUT / "repository"
repository.mkdir(exist_ok=True)
hello = next(p for p in packages if p["Package"] == "hello")
deb = (OUT / Path(hello["Filename"]).name).read_bytes()
(repository / "hello.deb").write_bytes(deb)
(repository / "Packages").write_text(
    "Package: hello\nVersion: " + hello["Version"] + "\nArchitecture: arm64\n"
    "Maintainer: Debian\nDepends: " + hello["Depends"] + "\nFilename: ./hello.deb\n"
    "Size: " + str(len(deb)) + "\nSHA256: " + hello["SHA256"] + "\nDescription: GNU hello\n\n")
entries["tmp/Packages"] = (stat.S_IFREG | 0o644, (repository / "Packages").read_bytes())

# Explicit parent directories precede files; links are emitted last so they
# can never redirect a subsequent extraction outside the new fixture root.
for name in list(entries):
    for parent in PurePosixPath(name).parents:
        if str(parent) != ".":
            entries.setdefault(str(parent), (stat.S_IFDIR | 0o755, b""))
ordered = sorted(entries, key=lambda n: (stat.S_ISLNK(entries[n][0]), n.count("/"), n))
with (OUT / "rootfs.pack").open("wb") as output:
    output.write(b"GOBLINFS" + struct.pack("<I", len(ordered)))
    for name in ordered:
        mode, data = entries[name]
        encoded = name.encode()
        output.write(struct.pack("<IIQ", len(encoded), mode, len(data)))
        output.write(encoded)
        output.write(data)
print(f"built {OUT / 'rootfs.pack'} ({len(entries)} entries)")
