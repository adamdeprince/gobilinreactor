# UML validation

The default APK now uses the pinned ARM64 UML kernel. Reports from earlier
custom-ABI builds remain historical evidence under `harness/results`; they do
not describe the current backend.

The 0.3.0 family-test APK and App Bundle results are in
[harness/results/family-beta](../harness/results/family-beta/README.md), including
startup/recovery controls, local diagnostic export and actual bundle-generated
split installation. Follow [the internal testing guide](internal-testing.md)
for the handoff and repeatable release commands.

Earlier app-zygote integration and signed APK results are in
[harness/results/app-zygote](../harness/results/app-zygote/README.md). These cover
unchanged Android monitoring policy, both host page sizes, the Samsung phone,
in-place upgrades and the managed network service. Earlier raw-child-hosting
results below are historical and do not establish this deployment's behavior.

Parallel-CPU and wake-lock results are in
[harness/results/uml-parallel](../harness/results/uml-parallel/README.md), covering
the eight-CPU Samsung SM-S928U and four- and eight-CPU API 36 emulators with
16 KiB and 4 KiB host pages. The phone passed the in-place APK upgrade, native
parallelism, package and background-operation checks. Earlier baseline UML
results in [harness/results/uml-20260919](../harness/results/uml-20260919/)
cover the Samsung SM-S928U and the 16 KiB emulator. All use a 16 KiB-page guest.

## Run the checks

```sh
# In the isolated Linux builder, as root:
python3 tests/uml_migration_test.py
python3 tests/deployment_upgrade_test.py

# With the APK and Android tooling prepared:
bash tests/harness_runner_test.sh
bash harness/run.sh DEVICE regression
bash harness/run.sh DEVICE emacs
bash harness/run.sh DEVICE recovery
adb -s DEVICE shell am instrument -w dev.goblinlinux.sentry/.TerminalAcceptance
python3 tests/terminal_lifecycle_test.py DEVICE
python3 tests/emacs_install_test.py DEVICE --output /tmp/goblin-emacs-check
python3 tests/terminal_keyboard_test.py EMULATOR

# In-place Android update with a changed kernel build and unchanged packages:
python3 tests/apk_upgrade_test.py DEVICE harness/build/goblin-sentry.apk --expect-kernel-update
```

The migration check verifies ownership, permissions including setuid bits,
hard links, virtual symlinks, socket nodes, usrmerge, interrupted promotions,
source preservation and corrupt metadata rejection. It requires root only in
the disposable Linux build VM; it does not alter the development host's users.

The deployment upgrade check runs the real deployment script and Debian tools
inside temporary chroots. It checks first installation, updates to unchanged
defaults, preservation of content/permissions/ownership/deletions/links, legacy
APK adoption, interrupted-update retries and idempotence. It also verifies that
custom sudoers errors do not block guest startup and empty DNS config survives.

The APK upgrade test installs with `adb install -r` over a running guest. It
compares the existing configuration, account files and package inventory,
checks uniquely named home/config probe files, and verifies that the disk image
was retained. It matches the installed kernel, extracted initramfs and running
PID 1 against the new APK; `--expect-kernel-update` additionally requires a
changed kernel binary and guest kernel build identifier. Probe files are removed
afterward. This test is for runtime/config upgrades with unchanged guest package
versions, and reads existing configuration without editing it.
On a disposable emulator, add `--exercise-managed-config` to temporarily edit
the sudo/repository files, change sudo permissions, delete the signing key and
empty DNS configuration before the update. Their originals are restored afterward.

The actual APK's guest checks verify the Linux release, ext4, `goblin`,
passwordless `sudo su -`, the signed repository and consistent dpkg state. They
create a sparse 10 GiB file, run 80 concurrent children, and in extended modes
touch 576 MiB of memory and exercise threads, sockets, apt, HTTPS, GCC and Emacs.
They do not intentionally exhaust the device. Modes `persistent` and `developer`
select the extended suite; `emacs` also installs/runs emacs-nox.

The Android instrumentation checks real rendering and input: Bash, Unicode,
less, Vim, foreground/background query replies, graphics-request handling,
pinch and PTY resizing, persisted font size, Ctrl/Alt/Esc/Tab/arrows, account
creation, root administration, thirteen terminals and detached HTTP services
surviving closure of every terminal. The separate keyboard test uses a temporary
emulator uinput device to check hiding and restoring the extra-key row.

Lifecycle checks type through Android input, reconnect to a live shell/job,
close terminals, restart the app and verify durable guest files. The recovery
test force-stops a VM doing unsynced rename/write activity and verifies that a
previously fsynced file and the installed package database survive ext4 recovery.
An app force-stop is abrupt VM power loss: unsynced guest page-cache data is not
promised durable. A normal terminal exit keeps the kernel and its cache alive.

## Fresh deployment

Harness mode 5 creates its own `files/uml-fresh-check` environment, with no legacy
root. It bootstraps the packaged Debian seed and runs the core checks. This tests
account, sudo and repository deployment without deleting the installed guest.
Read its report at `files/uml-fresh-check/phase1-report.txt`; remove only that
test-owned directory after its VM has shut down. Start the harness from a stopped
app so an existing interactive VM does not retain the singleton runtime.

## Parallel CPUs and power

`python3 tests/uml_parallel_test.py DEVICE --output REPORT` uses GCC in the
running guest, or accepts `--binary PATH` for a static ARM64 build of the same
test on a fresh guest. It compares Android's present CPU mask against guest configured,
online and `/proc/cpuinfo` counts, then compiles `uml_parallel.c`. The native test
uses fast cross-core handoffs without syscalls to detect the original per-MM
serialization; it also reports serial/parallel throughput. It checks TLS on
every CPU, cross-core mmap/mprotect/munmap, 80 migrating threads, targeted signals,
and fork/exec while a sibling runs. Its deadlines belong only to the test runner.

`adb -s DEVICE shell am instrument -w dev.goblinlinux.sentry/.PowerAcceptance`
checks actual Android wake-lock acquisition, opt-out, re-enable, screen-off
background progress, release on shutdown and reacquisition on restart. It
restores the original keep-awake preference. Like terminal instrumentation, it
restarts the app; run it on a test environment. It does not automatically grant
Android's battery-optimization exemption or assert that a wake lock overrides
Doze.

`python3 tests/uml_worker_failure_test.py emulator-PORT --output REPORT` runs
only on a disposable emulator. It identifies a test process's new host MM from
mapping metadata in the isolated UID, using the emulator's `su 0` test shell,
then kills its mapping or execution worker during
concurrent mmap/mprotect activity. The guest process must terminate, every worker
of that MM must be reaped, and the same VM must remain responsive. This covers
host-side worker death while the guest MM lock has interrupts disabled.
It does not require phone root or change Android policy.

## Managed hosting and release checks

```sh
VARIANT=release bash harness/build.sh
bash harness/build-tests.sh
python3 tests/release_artifact_test.py harness/build-release/goblin-sentry.apk
adb -s DEVICE install -r harness/build-tests/goblin-tests.apk
python3 tests/uml_hosting_test.py DEVICE --mode soak --seconds 600 --output RESULTS
```

The separate test APK must match the installed target's signing key. Build it
with `SIGNING=debug TARGET_BUILD=harness/build` for the debug APK. Release builds
contain neither test instrumentation nor the harness. `release_matrix.py` checks
APK replacement, binary transport, networking, recovery, sustained multicore
execution and wake locks; `--ui` includes terminal rendering on an unlocked device.
Terminal rendering requires `less` and `vim` in the test guest. Install missing
test packages only in a disposable guest.

`uml_hosting_test.py` accepts `network`, `recovery`, `transport` and `soak`. It
records isolated workers, Android phantom records, worker cleanup and policy
before/after. The transport check repeatedly sends binary data larger than the
TTY queue and follows each burst with another request. This catches lost read
notifications after serial input throttling.

`ServicesAcceptance` mode `unplugged` requires physically removing USB power,
turning off the phone's screen for five minutes, and reconnecting afterward.
End phone calls before measuring idle behavior. Allow six minutes before
reconnecting to leave a margin around the five-minute measurement. Start this
instrumentation without `-w` so the launching USB shell exits independently:

```sh
adb -s DEVICE shell am instrument -e mode unplugged -e seconds 300 dev.goblinlinux.sentry.tests/dev.goblinlinux.sentry.ServicesAcceptance
adb -s DEVICE shell logcat -d -s goblin-battery:I
```

Wait for the new run's `READY` log before unplugging. After reconnection, check
that the same run logged `PASS`, then use `-w -e mode collect-unplugged` to retrieve
the saved report. Keep in-call measurements separate from idle measurements.
It checks actual Android battery state and a guest service heartbeat, without
simulating battery state or changing power policy. Mode `collect-unplugged`
returns the saved report after reconnection. A USB-powered screen-off test does
not establish this result. Backup/restore testing needs Android free space for
the compressed backup and an additional allocated disk copy; guest logical free
space does not measure that requirement.

## Optional startup after Android reboot

`tests/boot_start_test.py` installs the signed release on a disposable ARM64
emulator and performs three actual reboots. It checks default-off behavior,
opt-in through the terminal menu, background startup with a notification,
automatic execution of an enabled guest systemd service, the wake-lock opt-out,
duplicate boot delivery, and disabling the option before the next reboot.
It preserves existing guest data, removes its temporary service and file, and
leaves the option disabled. The emulator must already have a deployed guest,
start with this option off, allow ADB root, and unlock automatically:

```sh
python3 tests/boot_start_test.py emulator-SERIAL harness/build-release/goblin-sentry.apk \
    --output harness/results/boot-start-test
```

The script refuses physical-device serials. Its use of emulator ADB root only
observes the release app; it does not grant the app host privileges or change
Android's process or battery policies. PIN-protected first-unlock behavior and
physical-phone reboot behavior need separate tests.

## Scope

These checks validate the listed behavior. They do not certify every Linux
feature, every vendor Android policy, physical 16 KiB hardware, arbitrary service
managers or Google Play distribution. See [uml/README.md](../uml/README.md) for remaining integration
boundaries and [architecture.md](architecture.md) for the runtime design.
