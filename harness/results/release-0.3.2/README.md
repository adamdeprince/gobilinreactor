# GoblinReactor Linux® Environment 0.3.2 validation

2026-09-20. Version code 30002, ARM64, UML kernel build #17.

- APK SHA-256: `775406133203277719f98f2667a742cd4c470917dacace0a9f4c61ea84339712`.
- App Bundle SHA-256: `51be718f500069ba33670e5e154a53081b563fa92636c33a39a1ca9fd448f8be`.

## Change

The terminal menu now offers **Start Linux after reboot**, disabled by default.
After explicit opt-in, Android's `BOOT_COMPLETED` broadcast starts the existing
foreground Linux service after the first user unlock. The receiver is not
exported and is not Direct Boot aware. Linux files and preferences remain in
credential-encrypted app storage. No terminal activity is opened automatically.

The existing notification, shutdown action and Keep Linux awake preference
apply. Turning the option off leaves the current Linux instance running and
prevents startup on later reboots. Enabled guest systemd services start normally;
process memory, shell sessions and unsaved work are not restored. Android startup
denial is caught and recorded in local diagnostics without repeated attempts.

## Actual reboot tests

The [Android 12, 4 KiB report](android12/result.json) and
[Android 16, 16 KiB report](android16-16k/result.json) each cover three real
emulator reboots of this exact release APK:

- The default-off setting does not start Linux after reboot.
- Enabling the option in the actual menu persists across reboot and starts Linux
  with a foreground notification while leaving the terminal activity closed.
- An enabled guest systemd service runs automatically and records the guest boot
  identity in a persistent home-directory file.
- Keep Linux awake remains disabled when the user has opted out.
- A repeated boot broadcast retains the same Linux instance and does not run the
  test service a second time.
- Disabling the option leaves Linux running, then prevents automatic startup on
  the following reboot. Manual startup still works and preserves guest files.

The test restores the original wake preference, removes its temporary guest
service and file, and leaves startup-after-reboot disabled. Emulator ADB root
only observes the release app's private control socket; the app remains
unprivileged. These emulators unlock automatically. A PIN-protected first-unlock
test and a physical-phone reboot test were not performed. Android force-stop
and vendor battery restrictions can prevent broadcast delivery or startup.

The [Samsung update check](phone-update.json) verifies clean guest shutdown,
in-place installation of this exact APK, reopening the `goblin` home-directory
terminal and the new menu option displayed unchecked. The phone was not rebooted
and its automatic-start preference remains off.

Reproduce on a disposable, already-deployed ARM64 emulator with the option off:

```sh
python3 tests/boot_start_test.py emulator-SERIAL harness/build-release/goblin-sentry.apk \
    --output harness/results/release-0.3.2/recheck
```

## Packaging and existing compatibility

The [APK checks](release-artifact.json) and [App Bundle checks](bundle-artifact.json)
pass production signing, non-debuggable packaging, exclusion of test components
and secrets, native executable extraction, and 16 KiB ELF/ZIP alignment. The
[payload comparison](runtime-comparison.json) confirms all 94 native components
and runtime/deployment assets are unchanged from 0.3.1. The application ID and
signing identity are retained for updates that preserve existing guest data.

The broader [0.3.0 family test matrix](../family-beta/README.md) remains the
baseline for terminal, package management, kernel, networking, CPU and recovery
behavior. It was not repeated for this Android lifecycle change. Android 12+
with a 64-bit ARM64 Android installation remains the support target; Android
13–15, other physical manufacturers and long-term battery behavior still need
direct testing. The App Bundle has not been uploaded to Google Play.

The product-name sublicense still needs application and approval before public
distribution; see [the naming record](../../../docs/trademark.md).

Linux® is the registered trademark of Linus Torvalds in the U.S. and other countries.
