# Android APK update preservation

APK: `harness/build/goblin-sentry.apk`

SHA256: `ee7d1a998a82e0e1ae38542d608b474b3a56152b92af318e81ffa61d2b6c759f`

The Samsung SM-S928U (4 KiB Android pages) and disposable Android emulator
(16 KiB Android pages) both passed an in-place `adb install -r` upgrade from
the previous UML APK. The guest disk inode, 15 checked paths and all 188
installed packages were retained. The paths cover user documents, ordinary
configuration, accounts, sudo, APT, DNS and shell startup files. Existing phone
configuration was read without being edited. Uniquely named probe files were
removed after the checks.

Both guests moved from kernel build `#5` to `#6` of the same pinned
`7.2.0-rc4-goblin` source. This was a kernel replacement test, not a change of
upstream kernel revision. The installed kernel and extracted initramfs hashes
matched the new APK. The running guest PID 1 executable also matched the
updated initramfs manifest.

The emulator additionally retained administrator comments in the sudo/APT
configuration, changed sudo permissions, a deliberately deleted repository
signing key and an empty `/etc/resolv.conf`. Its original configuration was
restored after the test.

| Evidence | Result |
|---|---|
| [Phone APK upgrade](phone-upgrade.json) | Data/configuration retained; new kernel and init verified. |
| [Emulator APK upgrade](emulator-upgrade.json) | Same checks, plus edited/deleted configuration preservation. |
| [Phone core checks](phone-core.txt) | Accounts, passwordless sudo, ext4, sparse files, processes and package consistency. |
| [Fresh emulator deployment](emulator-fresh.txt) | Fresh Debian bootstrap and core checks on an isolated test disk, then test-disk cleanup. |

`tests/deployment_upgrade_test.py` passed all 11 cases in disposable chroots
inside the Linux builder. It covers fresh installation, updates to unchanged
defaults, edits/deletions/empty files, symlinks, directories, permissions,
ownership, hard links, older APKs without snapshots, interrupted-update retries
and repeated-startup idempotence. It uses the actual deployment script and
Debian utilities. The chroots use `/var/tmp` because the builder's `/tmp` is
mounted `nodev` and cannot provide their `/dev/null` device.

APK signature verification, 16 KiB zip alignment, shell/Python syntax checks
and `git diff --check` also passed. The temporary Linux builder and emulator
were shut down after validation; the updated phone remains on its original
Linux disk.
