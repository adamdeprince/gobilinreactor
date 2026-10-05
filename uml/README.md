# Android UML runtime

The production APK runs stable Linux **7.2.9** built for User-Mode Linux. The
official source archive, checksum and ARM64 port provenance are in [sources.lock.json](sources.lock.json). Goblin's code here
provides Android lifecycle, deployment, disk migration and terminal transport.
Linux implements syscalls, memory, credentials, filesystems, sockets and PTYs.

## Build

Use a case-sensitive Debian 13 ARM64 Linux builder with these packages:

```sh
apt-get install build-essential clang llvm lld flex bison bc libelf-dev libssl-dev \
  git cpio pkg-config python3 e2fsprogs busybox-static
export NDK_TOOLS=/path/to/android-ndk-r29/toolchains/llvm/prebuilt/linux-x86_64
bash uml/build.sh
```

The NDK contributes target headers and libraries. The builder's native LLVM
executes the build, so the NDK host package does not need to match the build CPU.
`build-kernel.sh` also accepts `UML_SOURCE` and `UML_OUTPUT`, and
`build-passt.sh` accepts `PASST_SOURCE`. On macOS, put the Linux source and object
tree inside a Linux VM rather than building through a case-insensitive share.
The stable archive is checksum-verified before extraction by `prepare-kernel.py`.
It applies the pinned ARM64 port in `port-patches/` without changing the upstream
release version. The separate Android and parallel-worker patches then apply. Patches in `patches/` must apply cleanly without fuzz; a repeat
build verifies the complete stack by reversing its applied prefix in staging
and applying the current stack before updating the source tree. `check-stub.py` rejects compiler-generated
references outside the stub's copied code page.
The NDK installation itself is never modified.

The packaged userspace stub declares 16 KiB ELF load alignment. Android FD
cleanup enumerates actual open descriptors instead of issuing `close_range`,
which Android 12's inherited seccomp policy kills. Initial workers close their
known mapping descriptors; shared-memory workers close their received FD batch.
No Android policy override or smaller descriptor limit is required.

Artifacts go to `uml/build/artifacts`. The guest initramfs includes a manifest
of Debian package versions and file hashes. The full builder adds source,
compiler, input and output hashes to `build-manifest.json`.

On the Android build host, prepare `fixtures/` and `terminal/` as described in
the top-level README, copy the artifact directory back if necessary, then run
`bash harness/build.sh`. The APK contains the kernel, its userspace stub, passt,
the initramfs and the deployment data. There is no alternate ABI backend in this
build. The kernel, stub and port-control executables use `.so` filenames so Android
installs them in the executable native-library directory. `libgoblinlauncher.so`
and `libgoblinuml-netservice.so` are JNI libraries for the managed host services.

## Storage and startup

`runtime.cpp` creates a sparse ext4 backing disk using the capacity of the phone's
app filesystem. First boot imports the existing `files/debian` tree through a
read-only exporter, or extracts the packaged Debian seed for a fresh install.
The old custom ABI's ownership xattrs and virtual hard links become real Linux
metadata and links. The original directory is retained as a migration backup.
The pending image becomes `files/uml/rootfs.ext4` only after guest setup succeeds,
syncs the disk and sends its ready message.

Subsequent starts run `e2fsck -p`; a grown host backing disk triggers a full check
and `resize2fs`. The guest uses ext4's journal. Android force-stop can terminate
Linux abruptly, so the next startup recovers the journal. The app's shutdown
action requests a guest sync and poweroff.

## Android app updates

The APK owns the UML kernel, its Android stub, networking and terminal helpers,
and initramfs (including guest PID 1). Every Linux start uses the installed APK's
native libraries and refreshes the initramfs from its assets, independently of
the guest deployment marker. An in-place Android package update can therefore
replace these components without replacing `files/uml/rootfs.ext4`. The new
components take effect when Linux next starts. The Debian seed is only unpacked
when creating a new disk; home directories, accounts, packages and existing
Linux configuration remain on the persistent disk.

`fixtures/deploy-guest.sh` handles bundled guest package/configuration updates.
It retains newer installed package versions and uses `dpkg --force-confold` for
bundled package upgrades. The Goblin sudo rule, repository source, signing key
and resolver defaults are tracked under `/var/lib/goblin/config-defaults`, with
their full `/etc/...` paths beneath it. An existing file receives a new default
only when its contents, permissions and ownership still match the previous
default and it is a regular file with one link. User edits, empty files,
symlinks, hard links and deliberate deletions remain intact. Updated packaged
defaults are available in that separate directory for review and manual merging.
The deployment does not validate or repair an administrator's custom sudoers
configuration during boot.

The first update from older APKs has no default snapshots to compare against.
It conservatively keeps existing configuration and missing files, then records
the shipped defaults for future comparisons. First-time seed/legacy disk
import translates only the known shipped DNS configurations; subsequent boots
do not recreate an empty or deleted `/etc/resolv.conf`.

## Resources and process lifetime

Guest RAM is sized from Android's physical-memory report and backed by memfd,
not a file on flash or a size-limited tmpfs mount. The disk is sparse and can
use the phone's storage capacity. Goblin imposes no smaller disk quota, RAM
window, task count, terminal count, CPU budget, output budget or wall-time limit.
Linux defaults and user-configured rlimits/cgroups remain normal Linux controls.
Guest root can change them. Android's app permissions, memory killer and actual
available resources remain outside the guest's authority.

The disk's logical free space can exceed Android's current free space because
Android and other apps share the underlying storage. It is thin provisioned,
not a reservation. Guest allocations can exhaust host memory or storage.

An Android foreground service binds one app-zygote isolated service for the UML
kernel and all its workers. The launcher receives the persistent disk, initrd,
boot archive, disk lock and transport channels as open descriptors through Binder.
UML's `fd:N` host-file support duplicates them directly, and `uml_dir=none` avoids
host PID/socket files. APK boot assets arrive on a read-only second block device;
the guest unpacks them into tmpfs, without a live hostfs mount or a guest quota.
No Android phantom-monitor setting is changed. The exemption observed for
app-zygote services is Android implementation behavior, not a compatibility promise.

Networking runs inside another Android-managed service with the normal app UID,
so it can create Internet sockets. The isolated kernel host has no such permission.
Host shutdown reaps the UML process tree before releasing the shared disk lock;
status pipes and Binder death handling report failures to the foreground service.

Debian systemd reaps orphaned children and supervises services; Goblin's guest
broker hosts a PTY for each terminal. Closing a terminal closes its PTY
and sends the shell SIGHUP; normal Linux job control applies. Detached services
survive closing every terminal. Android force-stop ends the entire app and VM.

The service holds the `Goblin:Linux` partial wake lock from startup through
shutdown, with no elapsed-time cutoff. The terminal menu's **Keep Linux awake**
preference defaults to enabled and releases/reacquires the lock without
restarting Linux. Shutdown, startup failure and service destruction release it;
Android also releases it if the app dies. **Android battery settings…** requests
an exemption only when the user selects it. Android Doze can otherwise suspend
an app despite its wake lock; the guest cannot override that
[host policy](https://developer.android.com/training/monitoring-device-state/doze-standby).

## CPUs and parallel threads

The kernel enables SMP. On each start, `runtime.cpp` passes Android's
`_SC_NPROCESSORS_CONF` as `ncpus=`, including temporarily offline hardware cores.
Linux exposes that many online CPUs in `/proc/cpuinfo` and sysfs. Android's
scheduler chooses which host cores execute the workers.

The ARM64 seccomp backend uses independent execution workers for each guest MM
and CPU that actually runs it. Workers share the host address space with
`CLONE_VM`; each has its own signal stack, registers, TLS state, control socket
and futex. Guest tasks stay on their selected virtual CPU for each userspace
handoff. This removes the original per-MM turnstile around guest instructions;
ordinary Linux scheduling still time-slices tasks when they outnumber CPUs.

A parked worker serializes mapping commands. It applies mapping batches before
releasing the MM lock. Permission revocation and unmapping synchronously remove
host mappings from the shared MM, so every running worker observes them before
Linux returns or frees the pages. Faults republish current guest PTEs. Worker
pages are allocated lazily and retained until the host reaps the corresponding
process. A worker failure terminates the affected MM. The generic ptrace and
non-ARM64 backends retain their upstream execution model.

## Components and current boundaries

| File | Responsibility |
|---|---|
| `runtime.cpp` | Managed-service handoff, disk ownership, boot assets and framed serial/PTY transport. |
| `launcher.cpp` | Isolated kernel launch, worker supervision and disk-lock lifetime. |
| `network-service.c` | Managed app-UID passt event loop. |
| `guest.cpp` | Initramfs setup, systemd boot and guest PTY/account broker. |
| `archive.cpp` | Read-only legacy filesystem conversion and boot asset transport. |
| `patch-kernel.py`, `patches/` | Memory-backed RAM, shared-MM workers, descriptor-based boot files and serial flow control. |
| `patch-passt.py` | Adapt passt host setup and its library event loop to Android. |
| `control.cpp` | Private app-UID diagnostic client used by acceptance tests. |

This is an external ARM64 UML port rather than an upstream-supported ARM64 UML
release. Guest applications execute ARM64
instructions natively. Debian systemd runs as PID 1, with user services and
PAM/logind sessions. Passt supports IPv4/IPv6 and configurable TCP/UDP forwarding
to Android localhost or the LAN. Guest DNS at `10.0.2.3` uses Android's resolver;
startup preserves administrator changes to resolver configuration.
The root account controls the Linux guest, not Android's UID,
SELinux policy or hardware drivers.

## Validation

`tests/uml_migration_test.py` runs as root inside the disposable Linux builder.
`uml/acceptance.sh` runs inside the actual APK's guest. It checks ext4, accounts,
sudo, sparse files over 8 GiB and more than 64 processes. Extended modes exercise
apt, HTTPS, GCC, threads, sockets, a touched allocation over 512 MiB and Emacs.
These tests do not intentionally exhaust the phone. The existing Android
instrumentation also checks thirteen terminals, real PTY resize, Unicode,
pinch, modifiers, graphics handling, accounts and detached service survival.

For a running debug APK:

```sh
python3 tests/uml_control.py DEVICE exec 'uname -a; free -h; df -h /'
python3 tests/uml_control.py DEVICE exec '/bin/sh /run/goblin-host/acceptance.sh 4'
python3 tests/uml_parallel_test.py DEVICE
adb -s DEVICE shell am instrument -w dev.goblinreactor.sentry/.TerminalAcceptance
adb -s DEVICE shell am instrument -w dev.goblinreactor.sentry/.PowerAcceptance
```

The parallel test compares host and guest CPU counts, performs syscall-free
cross-core communication and compares serial/native pthread CPU work. It also
checks shared mapping revocation, TLS, targeted signals, 80 migrating threads,
and fork/exec with a running sibling. Power instrumentation checks Android's
actual held locks, opt-out/re-enable, background screen-off progress, shutdown
and restart. Instrumentation restarts the debug app; use a test environment.

The private control socket gives the owning app UID guest-root access, consistent
with the app's explicit root-terminal menu. It is not a network login service.
