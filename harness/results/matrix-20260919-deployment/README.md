# Deployment features validation — 2026-09-19

Final APK SHA256:
`13b2d2e52fe7d16410ce5dd527a00cdeb9add33dcc4d0b891b6d0c64226f94fe`

The [final validation](validation.json) passes. [Installed APK hashes](installed-apks.json)
match on the Samsung and both emulators. Existing Debian accounts and files were
preserved during deployment.

| Target | Native pages | Passed checks |
|---|---:|---|
| Samsung SM-S928U, API 36 | 4096 | Regression, persistent Debian/deployment, developer workflows, recovery, 18 terminal checks, 10 lifecycle checks |
| Android ARM64 emulator, API 36 | 16384 | Same six suites, plus 5 keyboard hotplug checks |
| Additional Android ARM64 emulator, API 36 | 4096 | Regression, persistent Debian/deployment, 18 terminal checks, 5 keyboard hotplug checks |

The terminal suite verifies pinch in both directions, changed guest PTY dimensions,
saved font size after activity recreation, passwordless `sudo`, `sudo -i`, execution
as another user, and the calling shell retaining its unprivileged identity. The
persistent suite checks repeatable APK migrations and preservation of existing
accounts and a home-directory fixture. Native set-ID, secure-exec, resource, PTY
and Unix-socket regression cases cover the compatibility needed by Debian sudo.

The keyboard tests create temporary USB alphabetic devices through Android uinput,
check the extra keys disappear, type a shell command, and verify disconnect restores
the row. These run on both emulator page sizes; no physical Bluetooth keyboard was
used. The tests do not change Android input settings.

The original [matrix.json](matrix.json) retains one phone lifecycle runner failure:
in landscape with the IME open, the popup needed scrolling to reach Close terminal.
The host runner was corrected to scroll its ListView, and only that suite was rerun
on the unchanged APK. Its [final report](R5CX3468YQR/lifecycle-final.txt) passes all
10 checks. The failed attempt and screenshots are retained. The failed attempt's
uniquely named guest fixture was removed afterward.

Screenshots: [phone terminal](R5CX3468YQR/kitty.png),
[16 KiB terminal](emulator-5556/kitty.png),
[keyboard connected](emulator-5556/keyboard.png).
Additional 4 KiB reports are under
[deploy-20260919](../deploy-20260919/README.md).

Local verification passed: seven harness-runner cases, Python syntax checks,
`git diff --check`, and APK asset/package checksum verification. Sudo and its policy
are packaged guest migrations; no build-machine account or sudo configuration was
changed.
