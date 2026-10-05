# Emacs installation repair — 2026-09-19

Final APK SHA256: `277af60fb424f36035a209c6ed9dc8181ff36606af4bbfbefbb0057178fc37e6`.

The phone's original `sudo apt install emacs-nox` ran out of Goblin's 1 GiB logical
guest quota during unpacking. Android still had about 121 GiB free. The
[original APT log](apt-term-before.txt), [storage measurement](android-storage-before.txt)
and [interrupted package state](packages-before.json) are retained.

The deployed runtime now has an 8 GiB guest quota. `statfs` and `fstatfs` cap reported
space by both the quota and Android backing storage. This does not allocate an
8 GiB image. The disk fixture checks rejected growth, quota-aware reporting,
truncation, and deleted files still held by an open descriptor.

After successful unpacking, Emacs exposed a second issue: glibc's no-follow chmod
fallback uses an O_PATH descriptor through `/proc/self/fd`. Goblin previously
rejected that operation as a procfs write. The fix changes the pinned guest inode
with normal ownership checks. Native tests cover direct procfs chmod, the libc
fallback, an unlinked inode, denial for a nonowner, and continued protection of
procfs data. APT's PTY reuse also exposed stale controlling-session ownership;
the runtime now releases it and signals the foreground group when the session
leader's final thread exits. A native test reacquires the same PTY in a new session.

The phone's incomplete dependency transaction needed `apt --fix-broken install`
without package names before installing Emacs. Both steps use `--no-remove`.
The [final repair and batch tests](R5CX3468YQR/emacs-headless.txt) pass, including
clean dpkg audit, Lisp evaluation, no-follow permissions, byte compilation and
loading the compiled Lisp file. The phone reports 8.0 GiB capacity, about 1.2 GiB
charged usage and 6.9 GiB available. Emacs 30.1 and its dependencies are
[fully installed on both devices](installed-apks.json); Purrfect is absent.
Emacs is not bundled or preinstalled by ordinary guest deployment.

[Validation](validation.json) records exact APK identities for each check:

| Target | Kernel | Package/runtime checks | Terminal |
|---|---|---|---|
| Samsung SM-S928U, 4 KiB | [Pass on runtime candidate](R5CX3468YQR/regression-runtime-candidate.txt) | [Final APK Emacs repair and batch](R5CX3468YQR/emacs-headless.txt) | [Final APK Emacs editing](R5CX3468YQR/emacs.txt) |
| ARM64 emulator, 16 KiB | [Final APK pass](emulator-5556/regression.txt) | [Final APK persistent suite](emulator-5556/persistent.txt), [candidate Emacs batch](emulator-5556/emacs-headless-runtime-candidate.txt) | [Final APK 19 checks](emulator-5556/kitty.txt), [candidate Emacs editing](emulator-5556/emacs-ui-runtime-candidate.txt) |

The runtime candidate is
`b7fa7a0de95e4d7f7e8febd6bb740779bf217e68f656ab4ee48e010915c200f9`.
The final build changes only the Emacs test's two-step package-repair sequence;
the kernel and terminal implementation are identical. Emacs editing tests type
through Android input, save a file with Ctrl-X Ctrl-S, and exit with Ctrl-X Ctrl-C
to the original regular-user shell. The [phone screenshot](R5CX3468YQR/emacs.png)
and [emulator screenshot](emulator-5556/emacs.png) record the saved buffers.
The terminal suite also verifies interactive `sudo su -`
and detached-server survival after every terminal closes.

Intermediate reports are retained. The initial storage-only build exposed the
chmod error; the first phone repair command exposed the required APT sequencing.
The early host UI runner incorrectly treated `adb exec-out` missing-file output as
a completed status file. The checked-in runner uses the shell protocol's remote
exit status and waits for a complete status line. No package installation ran in
those early host-runner attempts.

Reproduce package repair with `harness/run.sh SERIAL emacs`. Reproduce the screen
check with `python3 tests/emacs_install_test.py SERIAL --output REPORT_DIRECTORY`
on an unlocked device. The seven local harness-runner cases, Python compilation,
shell syntax and `git diff --check` also pass. Developer, recovery, keyboard
hotplug and full Android lifecycle suites were not repeated for this patch.
