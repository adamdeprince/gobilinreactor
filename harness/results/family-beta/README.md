# Goblin Linux® Environment 0.3.0 test evidence

2026-09-20. Version code 30000, ARM64, UML kernel build #17.

- APK SHA-256: `d1a2f3d68c1ca78363464a67a17ba1e55a6ede7bb1fc5f44c220c158bd8cd119`.
- App Bundle SHA-256: `e3f88bf9c27c88fe2a8466e04cb5cfacb6e68f227a216449cc250bb23fa258ca`.
- Kernel SHA-256: `c02a09463aa5120127362ec60e5ddbdfad320f34ece5ac5a88fbcd3aa633aa86`.

The [APK inventory](release-artifact.json) verifies production signing, a
non-debuggable release, absence of test components and keys, and ZIP alignment.
The [bundle inventory](bundle-artifact.json) checks real generated splits,
native executable extraction, all ELF load segments at 16 KiB alignment, and
exact agreement with the APK's runtime and assets. The original signing
identity and Android application ID are retained.

## Final build

| Device / environment | Accepted checks |
| --- | --- |
| Samsung SM-S928U, Android 16, 4 KiB pages, 8 CPUs | [Upgrade, transport, network, 120-second native multicore workload and power handling](phone/result.json) |
| Android 16 emulator, 4 KiB pages, 8 CPUs | [Same checks plus product recovery UI](emulator4k/result.json) |
| Android 16 emulator, 16 KiB pages, 4 CPUs | [Actual App Bundle split upgrade, transport, network, 120-second multicore workload, power and product UI](emulator16k/result.json) |
| Android 12 emulator, 4 KiB pages, 4 CPUs, 2 GiB RAM | [Fresh deployment and product UI](android12/product.txt); [transport, network, 120-second multicore workload and power](android12/result.json); [full terminal suite](android12/terminal.txt); [passwordless sudo, signed apt update/install and hello execution](android12/apt-install-synced.txt) |
| Physical keyboard hotplug, 16 KiB emulator | [Keys hide and reappear; real input executes in the shell](emulator16k/keyboard.txt) |
| Disk archive failure handling, isolated Linux builder | [Sparse data, corruption, storage exhaustion, stale imports and previous disks](disk-backup.txt) |
| Inherited descriptor cleanup, isolated Linux builder | [Sparse high descriptors, multiple enumeration batches and retained handles](fd-cleanup.txt) |

Product checks exercise the startup panel, Android screen timeout, local help
and notices, actual storage reporting, and diagnostic export that excludes
terminal and guest-file canaries. Invalid restore keeps the same disk and data,
reports its outcome persistently, and restarts the installation. Clean shutdown
can reopen the same installation. Terminal appearance was inspected on the
[Samsung](phone/terminal.png) and [16 KiB emulator](emulator16k/terminal.png).

## Earlier candidate and failures

The candidate APK `24ac6d82fbf43cc98a76ef6e2dbbc1418c81a1d643df15167a74e3ecaf125fa5`
passed the complete [4 KiB UI/recovery matrix](candidate-emulator4k/result.json),
[Samsung recovery matrix](candidate-phone/result.json), and
[16 KiB product checks](candidate-emulator16k/product.txt). The final kernel adds
portable cleanup in its initial worker and seccomp probe for Android 12; all
other packaged runtime and product behavior remain the same.

Android 12 initially killed the launcher for invoking `close_range`. The final
launcher and kernel enumerate actual open descriptors or close the known worker
mapping/received descriptors, without changing Android policy or imposing an FD
limit. The ELF stub also now declares 16 KiB PT_LOAD alignment.

The first slow-device deployment exceeded the test's old two-minute wait; the
retry completed with a ten-minute test deadline. Product runtime has no deadline.
The fresh guest needed Python, less and Vim for acceptance tests; those packages
were installed only in the disposable test guest, not added to deployment.
An apt-index check initially lost unflushed data when Android ended
instrumentation; setup now syncs before that explicit abrupt VM teardown.
Dialog tests were corrected for Android's uppercase button labels, and power
checks wait for the login prompt before sending PTY input.

## Scope

Android 12 or newer on ARM64 is the support target; actual coverage here is
Android 12 and Android 16. Android 10–11, other physical manufacturers, and real
16 KiB phones are unvalidated. These checks do not establish a week of abuse,
long-term battery consumption, deep Doze behavior, or recovery from every OOM.
The earlier [physical battery test](../app-zygote/README.md) remains separate.
Android process monitoring, SELinux enforcement and battery exemptions were not
relaxed. Emulator ADB root was used to read test diagnostics, not to grant the
app privileges. Linux resource limits remain under the guest user's control.

The public product-name sublicense still needs application and approval; see
[the naming record](../../../docs/trademark.md). Play distribution is also pending.

Linux® is the registered trademark of Linus Torvalds in the U.S. and other countries.
