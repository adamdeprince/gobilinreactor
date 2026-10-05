# GoblinReactor 0.3.7 preview validation

October 5, 2026. Version code 30007, application ID
`dev.goblinreactor.sentry`, ARM64, stable 7.2.9-goblin kernel.

Menus, notification actions, runtime status, maintenance prompts and help now
use neutral environment labels. Documentation matches those controls. About
and the download page include upstream trademark ownership and independence
notices. The app retains its package identity and signing key.

## Artifact checks

- [APK](release-artifact.json): release signature, public package identity,
  stable kernel, non-debuggable packaging, no test components or signing
  secrets, and 16 KiB alignment.
- [App Bundle](bundle-artifact.json): signed generated splits preserve native
  executable extraction, alignment, runtime and asset hashes.
- [Comparison with 0.3.6](runtime-comparison.json): 262 APK entries, including
  the kernel, boot image, guest deployment, logo and 232 upstream notices,
  are byte-for-byte unchanged. Native adapters were rebuilt for message changes.
- Corresponding-source packaging checks every APK source input against the
  release commit. Handoff checks verify source contents, notice equality and
  distribution checksums before public staging.

## Emulator checks

The signed APK updated the existing 0.3.6 installation on the Android 16
emulator with 16 KiB host pages, preserving its installed environment.
[Product acceptance](emulator16k/product.txt) passed help and packaged notices,
diagnostic export and privacy canaries, actual phone storage reporting, invalid
backup rejection without disk replacement or data loss, persistent maintenance
results, and clean shutdown/restart through **Open environment** with guest data
preserved.

[Visible menu inspection](emulator16k/menu-labels.json) checked every menu row,
the renamed shutdown confirmation, restart control and foreground notification
action. The emulator initially had notification permission disabled; it was
enabled for that check. No menu row uses Linux or Debian as its label.

## Scope

This release changes app copy and trademark presentation. The unchanged kernel
and guest assets retain the validation recorded for 0.3.6. This run does not
repeat fresh deployment, the full network/terminal/power matrix, physical phone
installation, or Android 12 testing. Android 13–15 and other physical
manufacturers still require direct coverage. Android 10–11 remain experimental.
This is a sideload preview; no Google Play approval is claimed.
