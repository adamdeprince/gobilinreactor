# App-zygote integration evidence

2026-09-20. Android 16 on Samsung SM-S928U (4 KiB host pages, eight CPUs),
`goblin-parallel4k` (4 KiB/eight CPUs), and `goblin-deploy16k` (16 KiB/four CPUs).
The guest kernel uses 16 KiB pages in every case.

The final signed APK is version 0.2.1, code 20001, kernel build #14. Its SHA-256 is
`a2c0b5ca86361de8827cbc0f7deda84c5e2c74a13a13109c6b1a64dd37c02c14`.
[Release inventory](final-release-artifact.json) records native component hashes,
signing, non-debuggable packaging, absence of tests/secrets and 16 KiB alignment.
[Kernel patch checks](kernel-patches.txt) confirm clean and repeatable application
and exact agreement with the compiled tree.

## Final kernel evidence

| Check | Result / report |
| --- | --- |
| Phone APK replacement preserves user files, ownership, config and package versions | [PASS](phone-final/upgrade-verify.txt) |
| Phone repeated large binary transfers and follow-up requests | [PASS](phone-final/transport.txt) |
| Phone IPv4/IPv6 TCP/UDP, 260 listeners, rollback, DNS, PAM/logind and server persistence | [PASS](phone-final/network.txt) |
| Phone rendering, sudo login shells, Unicode, less/Vim, pinch, keys, accounts and thirteen terminals; original font preference restored | [PASS](phone-final/terminal-preference-restored.txt) |
| Phone wake-lock toggle, screen-off progress, shutdown and restart | [PASS](phone-final/power.txt) |
| Phone after the call ended: 300 seconds physically unplugged with screen off, 301 power samples, 281 heartbeat increments, same Linux instance | [PASS](phone-final/battery-idle/metadata.json) |
| Final signed APK, phone: 600-second multicore soak, 110 iterations, peak 77 observed workers, no phantom records or surviving workers | [PASS](phone-final/soak/metadata.json) |
| Final signed APK, 4 KiB emulator: upgrade, transport, network, recovery, 120-second soak, power and UI | [PASS](emulator4k-final/result.json) |
| Final signed APK, 16 KiB emulator: upgrade preservation, transport and network | [PASS](emulator16k-final/result.json) |
| 16 KiB host serial backpressure regression, 16 binary round trips | [PASS](emulator16k-transport/metadata.json) |
| 16 KiB host six mapping/execution worker deaths; same VM remains responsive | [PASS](emulator16k-worker-failure.txt) |
| 16 KiB host backup, restore, previous-disk recovery and rescue | [PASS](emulator16k-recovery-final/metadata.json) |
| 16 KiB host fresh packaged seed, account/sudo/repository and process tests | [PASS](emulator16k-fresh.txt) |
| 16 KiB host abrupt force-stop, restart and synced-data recovery | [PASS](emulator16k-force-stop.txt) |
| 16 KiB host keyboard hotplug, actual input and extra-key visibility | [PASS](emulator16k-keyboard.txt) |

Raw reports are retained for failed attempts as well as successes; the table
identifies the accepted runs. The [physical idle battery run](phone-final/battery-idle/metadata.json)
passed after the user ended the call: 300 seconds with 301 Android power samples
and 281 guest heartbeat increments. The same Linux instance survived,
battery-optimization exemption was false, and monitoring policy stayed unchanged.
The earlier [in-call run](phone-final/battery/metadata.json) also passed, with 282
heartbeat increments; it remains a separate result. Waking the screen on
reconnection occurred after the measured interval and did not invalidate it.
No battery state was simulated. These bounded measurements do not establish
indefinite operation or demonstrate that Android entered deep Doze.
The [final phone summary](phone-final/result.json) ties its successful checks to
the signed APK hash.

## Earlier integration evidence

The initial integrated kernel #13 APK predates the serial backpressure fix.
[Phone soak](phone-soak/metadata.json) passed 600 seconds and 110 native stress
iterations, with a sampled peak of 70 workers and no matching phantom records.
The [4 KiB signed release soak](emulator4k-release/soak.txt) passed 600 seconds and
107 iterations. Its [recovery](emulator4k-release/recovery.txt) also passed.
[16 KiB networking](emulator16k-network/metadata.json) passed, with a sampled peak
of 35 isolated workers. The initial phone upgrade retained the same disk and all
199 installed packages; [its report](phone-upgrade.json) also matches the changed
kernel, initramfs and guest agent against the installed APK.

The standalone [Samsung probe](../../../probe/appzygote/results/samsung-zygote/metadata.json)
completed 600 seconds with 64 native workers. Earlier 180-second emulator probes
and the ordinary-service phantom-trimming control remain in
[the research evidence](../../../probe/appzygote/README.md).

## Interpretation and failed attempts

Observed runs retained `max_phantom_processes=32`, an unset monitoring override,
an empty persistent override, and SELinux `Enforcing`. No monitor/battery-policy
setting was changed. Full-guest observers found no matching phantom records and
no sampled workers left after host termination. Counts are sampled workload sizes,
not product limits. Tests do not intentionally OOM the phone.

The first 16 KiB recovery run ran out of Android storage while retaining a test
backup and previous disk. After removing only the failed test's snapshot/probes,
recovery passed; user data and the legacy migration backup were retained. The
serial stall during an early worker-failure attempt happened before any fault
injection; kernel patch 0003 fixes the missing notification after TTY throttling.

The first 4 KiB terminal run lacked `less`; the disposable guest received `less`
and `vim-tiny` as test prerequisites. The first phone terminal run attempted to
delete its temporary account before systemd stopped the user manager; the test
now stops that account's slice explicitly. A report collector also expected the
wrong terminal pass marker; the actual marker is `KITTY ACCEPTANCE PASS`.
The final emulator UI attempt exposed another test race: a new PTY was marked
open before Debian login completed its input flush. The test now waits for the
shell prompt before sending commands, and its rerun passed. Pinch tests restore
the original font preference exactly, rather than retaining rounded zoom changes.
These failures and the earlier observer that read only truncated process names
remain visible in their raw report directories.

A screen-off check while USB supplies power is separate from physical unplugged
operation. Physical 16 KiB hardware, other vendors/Android releases, indefinite
Doze/network operation, OOM behavior and Google Play distribution are not certified.
The Android app-zygote exclusion is observed implementation behavior rather than
a documented compatibility promise. See [the research](../../../docs/app-zygote-research.md).
