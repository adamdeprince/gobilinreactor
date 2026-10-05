# Hardware CPU count, parallel threads and wake locks

Final APK: `harness/build/goblin-sentry.apk`

- APK SHA-256: `095ea566e560c139460d2b91e7a40904e6dcff26d4797b13377bd2870eed3c82`
- Kernel: `7.2.0-rc4-goblin #11 SMP Sun Sep 20 00:30:33 UTC 2026`
- Kernel SHA-256: `29fda23f21b1af53e5bcd966af2bcdf8bb0083f4f790c03b0502d265e4c85e04`
- Linux source: `8897487c52233cd00cf2850008ca068892f1ae91`
- Shared-MM patch SHA-256: `7f3a9d505854cec5e121f2d55430402f3125c0a2bb258e762c46c6a9819262ee`

Every start passes Android's configured hardware CPU count to UML. The ARM64
seccomp backend has separate execution workers sharing each guest address space,
so threads in one process execute concurrently. A dedicated worker applies
mapping changes, including synchronous cross-core permission revocation. Workers
are allocated on demand and reaped after exit.

The foreground service holds an untimed partial wake lock while Linux runs.
The user can toggle **Keep Linux awake** without restarting Linux. Shutdown and
failure release the lock. Android battery exemption remains an explicit user
choice in the terminal menu.

## Final APK verification

| Environment | Result | Evidence |
|---|---|---|
| Samsung SM-S928U, API 36, 4 KiB host pages, eight CPUs | Guest exposes all eight CPUs; native parallel threads, TLS, signals, migration, memory protection and fork/exec pass. CPU benchmark: 5.58x speedup. | [Parallel test](samsung-parallel.txt) |
| Samsung APK upgrade from kernel #6 to #11 | Same persistent disk; 15 probed files/configuration paths and all 189 installed packages preserved. | [Upgrade test](samsung-upgrade.json) |
| Samsung package checks | Accounts, sudo, sparse files, 80 processes, allocation above 512 MiB, sockets, UDP/TCP DNS, apt, GCC, HTTPS and Emacs pass. | [Package test](samsung-packages.txt) |
| Samsung background operation | Actual Android partial wake lock active; Linux heartbeat advances while the terminal is defocused and the same VM remains running. | [Background test](samsung-background.txt) |
| API 36 ARM64, 16 KiB host pages, four CPUs | Guest exposes four CPUs; concurrent native threads, TLS, signals, migration, mapping protection, fork/exec pass. CPU benchmark: 4.09x speedup. | [Parallel test](emulator-parallel.txt) |
| API 36 ARM64, 4 KiB host pages, eight CPUs | Guest exposes eight CPUs; the same regressions pass using a static ARM64 test executable in a fresh Debian installation. CPU benchmark: 6.65x speedup. | [Parallel test](emulator-4k-eight-cpu.txt) |
| Four-CPU emulator, injected worker deaths | Six alternating mapper/execution-worker kills terminate only the affected MM; its workers are reaped and the same VM remains responsive. | [Recovery test](emulator-worker-failure.txt), [kernel log](emulator-worker-recovery-boot.txt) |
| Four-CPU emulator, package checks | Accounts, sudo, large sparse files, 80 processes, allocation above 512 MiB, sockets, UDP/TCP DNS forwarding, apt, GCC, HTTPS and Emacs pass. | [Package test](emulator-packages.txt) |
| Eight-CPU emulator, fresh installation | Core kernel, ext4, account, sudo, sparse-file, process and signed-repository checks pass. | [Core test](emulator-4k-core.txt) |
| Both emulators, Android power instrumentation | Actual OS lock acquisition/release, opt-out, background progress with screen off, shutdown and restart pass. | [16 KiB](emulator-power.txt), [4 KiB](emulator-4k-power.txt) |
| APK upgrade from kernel #10 to #11 | Same persistent disk; 15 probed paths and 188 installed packages preserved, including edited and deleted managed configuration. | [Upgrade test](emulator-final-upgrade.json) |
| Clean pinned Linux source | All 22 modified files apply cleanly and idempotently; resulting source matches the compiled tree. | [Patch test](patch-application.txt) |

The native handoff test has two CPU-affine threads communicating without
syscalls: 999/1000 and 998/1000 handoffs completed within 100 microseconds on the
four- and eight-CPU emulators. Timings are measurements from these runs, not
performance guarantees for a phone. The SMP-only kernel with the old shared
execution worker timed out on this test.

[Terminal instrumentation](emulator-terminal.txt) passed on kernel #10 with the
same final frontend: interactive `sudo su -`, Unicode, less/Vim, pinch and PTY
resize, activity recreation, colors, modifier/arrow keys, thirteen terminals and
detached-server survival. Kernel #11 additionally fixes initial mapper PID
publication during startup; the frontend is unchanged.

The worker-recovery test initially rejected a valid target because an exited
transient process's RAM offset had been reused. The corrected test identifies
the disposable MM by its marker mapping and newly created host PIDs; all six
rounds above passed. This change affects only the emulator test.

After USB reconnection, the final APK was installed on the Samsung SM-S928U and
the physical-phone checks above passed. All 1000 native cross-core handoffs
completed within 100 microseconds. [The final boot log](samsung-boot.txt) contains
no kernel panic, warning or RCU-stall diagnostics. Goblin was returned to the
foreground with the Linux environment running.

The phone's power check leaves the VM and Android battery settings intact; it
checks the OS wake lock and background progress. Screen-off progress, preference
toggles, lock release on shutdown and reacquisition on restart were tested on
both emulators as recorded above.
