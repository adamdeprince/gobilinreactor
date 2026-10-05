# Source distribution and release builds

Each release handoff contains a signed APK and App Bundle, a matching
`GoblinReactor-VERSION-corresponding-source.tar`, `NOTICES.tar.gz`, component
notices, build instructions, validation results, and SHA-256 checksums.
Publish the source and notices beside the binaries with equal access and no
additional charge. These files are an accompanying source distribution, not a
promise to supply sources later. No signing keys are part of the handoff.

## What the source archive contains

- `project/`: the committed GoblinReactor source, local modifications, build
  scripts, Android resources, dependency locks, kernel configuration, and notices.
- `upstream/uml/`: pristine kernel and passt source at the pinned commits;
  a passt Git bundle supports its Git-based patcher without a network checkout.
- `upstream/terminal/`: upstream terminal, Python, rendering and build-tool archives.
- `upstream/python-sources/` and `upstream/python-recipes/`: exact dependency
  sources and Android packaging recipes/patches. The prebuilt dependencies used
  in the original build are also supplied for the normal terminal build path.
- `upstream/debian/`: `.dsc`, original source archives and Debian changes for
  every source version used in the seed, deployment, boot image and static
  C/C++ runtimes. Both old seed and newer deployment versions are retained.
- `SOURCE-REVISION`: the exact project commit. The adjacent `SOURCE-MANIFEST.json`
  hashes every source member and binds the source handoff to the release APK.

Third-party notices and license terms remain intact; see `LICENSE`, `COPYING`
and `THIRD_PARTY_NOTICES.md` in the project. The app incorporates GPLv3 kitty;
the independently built Linux kernel remains GPLv2. Original kernel changes
are licensed compatibly as stated in LICENSE. Android NDK and system toolchains
are separately installed build prerequisites; their distributed runtime notices
are included. No byte-for-byte cross-host reproducibility claim is made.

## Rebuild from the handoff

Verify SHA256SUMS before unpacking. On a case-sensitive filesystem:

```sh
mkdir goblin-source
cd goblin-source
tar -xf ../GoblinReactor-0.3.4-corresponding-source.tar
cd project
git init
python3 release/restore-source-cache.py
```

The local `git init` lets the Android builder enumerate source inputs for its
embedded provenance record; no upstream checkout or private repository is needed.

The source packages can be inspected or rebuilt without contacting the source
hosts. For a Debian component, use `dpkg-source -x PACKAGE.dsc` from its directory
under `upstream/debian/`, install its declared build dependencies and run
`dpkg-buildpackage` in the extracted tree. Review the source package's copyright
file and build rules for the relevant component. This includes the Debian
changes, rather than only the unmodified upstream tarball.

For modified Android Python libraries, unpack the matching archive in
`upstream/python-recipes/` and its source in `upstream/python-sources/`. Its
README, `build.sh`, `android-env.sh` and component patches describe the original
build, including the NDK version. The SQLite full source ZIP accompanies the
amalgamation used by its Android recipe. To relink the terminal with a changed
library, replace that library in the CPython prefix and rerun `terminal/build.py`
without reusing the old native build. The unmodified path uses the verified
prebuilt dependencies included in the handoff.

Build UML on Debian 13 ARM64 with a case-sensitive disk, following
`uml/README.md`. Also install `busybox-static` for the rescue image. The original
builder used clang 19.1.7, GCC 14.2.0, and the Debian runtime versions recorded in
`release/boot-packages.lock.json`. Exact kernel configuration is in
`release/kernel.config`. Restore the upstream trees locally:

```sh
mkdir -p uml/build
tar -xf ../upstream/uml/linux-8897487c52233cd00cf2850008ca068892f1ae91.tar.gz -C uml/build
git clone ../upstream/uml/passt.git.bundle uml/build/passt
export NDK_TOOLS=/path/to/android-ndk-r29/toolchains/llvm/prebuilt/linux-x86_64
bash uml/build-kernel.sh
bash uml/build-passt.sh
python3 uml/build-initramfs.py
```

The individual build scripts work with the supplied trees. The aggregate
`uml/build.sh` expects Git checkouts and is intended for a normal online source
checkout. The build scripts apply all local modifications. Install the recorded
Debian package versions when reproducing the original boot image; an intentional
change requires refreshing the boot-package audit and corresponding source locks.

On the Android build host, install Python 3.12+, Android SDK API 36, Build Tools
36.1.0, NDK 29.0.14206865, JDK 21, CMake, Ninja, pkg-config and a host C/C++ compiler.
Copy `uml/build/artifacts/` from the Linux builder into the project, then:

```sh
python3 fixtures/bootstrap.py
python3 fixtures/deployment.py
python3 terminal/build.py --offline
VARIANT=release SIGNING=debug bash harness/build.sh
```

The fixture builders download SHA-256-pinned Debian binary packages and never
execute them on the build host. Their corresponding sources are included for
modification/rebuilding. Rebuilt Debian packages require updating the fixture
locks to their versions and hashes. A rebuilt or modified boot image similarly
requires rerunning `release/audit-boot-packages.py` on its Linux builder, with
`initramfs-manifest.json` on stdin, into `release/boot-packages.lock.json`.

`SIGNING=debug` creates a local development key. It produces an installable
release-layout APK without access to our private signing key, but it cannot
update our signed release in place. Use a separate test device/profile or back
up data before replacing an installation. Public release signing keys are
intentionally not supplied. The app imposes no signature check on its own
rebuilt runtime components beyond Android's normal APK signing rules.

## Maintainer release procedure

1. Update `harness/version.json` and release notes. Preserve the app ID and key.
2. Refresh package/source locks only when inputs change. The resolver reads
   pinned binary package control records and Debian `Sources.xz` indexes in
   `release/build/cache/`. `audit-boot-packages.py` verifies actual boot-file
   hashes and records binary and source versions, including static runtimes.
3. Run `release/collect-sources.py` and `release/export-uml-sources.py` to collect
   verified archives. `--offline` verifies an already populated source cache.
4. Build the signed APK and bundle and run the artifact tests from
   `docs/internal-testing.md`. Save their JSON reports in the versioned evidence
   directory and distinguish packaging checks from device validation.
5. Commit the source and evidence, then run `release/package-sources.py`.
   It refuses a source commit that differs from the APK's recorded build inputs.
6. Run `harness/package-release.py`, then `tests/release_handoff_test.py dist/VERSION`.
   Tag the source commit only after the checks pass. Publish the whole handoff.

Large upstream archives, private keys, build outputs and local VM disks are
excluded from the project Git tree. Source locks and the handoff checks make
those separately shipped archives reviewable and verifiable.
