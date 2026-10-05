# Repository deployment and interactive sudo — 2026-09-19

APK SHA256: `2652500fed9d5e961360d3927afec2a16d46d3b122f830025a33f71f59f92c6b`.

`sudo su -` now acquires its nested PTY in sudo's new session with the correct
foreground process group. New PTYs start without an owner; terminal acquisition
enforces session ownership. The previous behavior retained the outer shell's
session, causing the interactive command to stop instead of presenting a root
login prompt.

The APK also deploys the Goblin Reactor source and its scoped signing key into
existing and fresh guest installations. No Purrfect package is bundled or installed.
The repository is configured offline; APT fetches and verifies its catalog during
an ordinary update. Host accounts and package configuration are not part of this
deployment.

| Target | Page size | Kernel | Debian and repository | Terminal | Lifecycle |
|---|---:|---|---|---|---|
| Samsung SM-S928U, API 36 | 4096 | [Pass](R5CX3468YQR/regression.txt) | [Pass](R5CX3468YQR/persistent.txt) | [19 checks](R5CX3468YQR/kitty.txt) | [10 checks](R5CX3468YQR/lifecycle.txt) |
| ARM64 emulator, API 36 | 16384 | [Pass](emulator-5556/regression.txt) | [Pass](emulator-5556/persistent.txt) | [19 checks](emulator-5556/kitty.txt) | [10 checks](emulator-5556/lifecycle.txt) |

[Validation](validation.json) records suite verdicts and the exact build.
[Installed APKs and guest files](installed-apks.json) match the
[packaged assets](packaged-assets.json). Purrfect was absent
[before deployment](optional-package-before.json) and remains absent afterward.
Both installations retained their existing Debian roots.

The regression suite includes 99 core and 49 VFS/ELF checks plus the native guest
execution scenarios. The set-ID fixture now checks an unowned PTY, acquisition
after `setsid`, and rejected acquisition/foreground changes from another session.
The Android terminal test enters the exact interactive command `sudo su -`, checks
`0:/root:/root`, exits, and verifies that the original unprivileged shell PID and
environment survive. Repository checks accept the signed ARM64 catalog and reject
the same repository with Debian's unrelated archive key; the negative check uses
an isolated APT configuration and list directory.

Final rendered screens: [Samsung](R5CX3468YQR/kitty.png) and
[16 KiB emulator](emulator-5556/kitty.png). Lifecycle reports additionally exercise
activity recreation, terminal closure, detached jobs and application restart.

The extra 4 KiB emulator's `*-candidate.txt` reports cover APK
`e9da342feef9ea1c17e93c717a9decc494bb0c22eaedc8ced7023d88a09e4371`.
That candidate passed regression and all 19 terminal checks. The final build adds
the generated source file to the deployment version digest and refreshes all
configured APT sources during the positive repository test, preserving Debian's
index cache. Final-build coverage comes from the two targets above. Developer,
recovery and keyboard hotplug suites were not repeated for this patch; their
[previous deployment results](../matrix-20260919-deployment/README.md) remain available.

To repeat these checks, run `harness/run.sh SERIAL regression`,
`harness/run.sh SERIAL persistent`,
`adb -s SERIAL shell am instrument -w -r dev.goblinlinux.sentry/.TerminalAcceptance`,
and `python3 tests/terminal_lifecycle_test.py SERIAL` with the built APK and an
unlocked display. Each command must report its explicit pass verdict.
