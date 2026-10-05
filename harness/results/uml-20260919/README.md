# UML deployment evidence — 2026-09-19

The exact final APK and kernel/helper/initramfs hashes are in [build.json](build.json).
The final APK is installed on Samsung SM-S928U `R5CX3468YQR` and was also tested
on the API 36 ARM64 16 KiB emulator. The original phone Debian tree is retained
as a backup; the active guest uses ext4.

- [Phone terminal](phone-terminal.txt) and [16 KiB terminal](emulator-terminal.txt): rendering, sudo login, Unicode, editors, pinch, keys, accounts, thirteen terminals and detached servers.
- [Phone Emacs](phone-emacs-ui.txt): sudo apt installation, package audit, batch mode, actual editing, save and exit.
- [Hardware keyboard](emulator-keyboard.txt): keys hide on connection, typing works, keys return on disconnection.
- [Runtime](phone-runtime.txt): deployed Linux release, guest page size, account, disk, RAM and Linux resource limits.
- [Fresh deployment](phone-fresh.txt), [packages/resources](phone-packages.txt), [migration](migration.txt), [crash recovery](emulator-recovery.txt) and [lifecycle](emulator-lifecycle.txt): component and integration checks described in the build record.

No test intentionally exhausted the phone. The fresh-install test used a separate
throwaway guest disk, which was removed after a successful shutdown. The build
VM was shut down after verification. Existing guest packages and files remain
available in the migrated environment.
