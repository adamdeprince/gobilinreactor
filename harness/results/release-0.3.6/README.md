# GoblinReactor 0.3.6 preview validation

October 5, 2026. Version code 30006, public application ID
`dev.goblinreactor.sentry`, ARM64, stable 7.2.9-goblin kernel.

This preview establishes the public package and signing identity before launch.
All 19 Java classes and the native terminal, launcher, network, lifecycle and
maintenance bindings use the GoblinReactor namespace. A separate RSA 4096-bit
GoblinReactor release certificate signs both the APK and App Bundle. Historical
internal test keys and installations are retained separately.

## Artifact checks

- [APK](release-artifact.json): correct public application ID, no old namespace
  in the manifest, DEX or native JNI exports; stable kernel banner; release
  signing; non-debuggable packaging; no test components or signing secrets;
  16 KiB alignment.
- [Bundle](bundle-artifact.json): generated splits retain all runtime assets,
  release signatures, executable extraction and 16 KiB alignment.
- [Runtime comparison](runtime-comparison.json): the kernel, boot image,
  deployment packages and all other assets except source metadata are unchanged
  from 0.3.5. Four native libraries were rebuilt for the namespace change.
- Corresponding-source packaging binds every APK source input to the release
  commit. Handoff checks validate archive contents, notice equality and every
  distribution checksum before public staging.

## Runtime checks

The release APK completed a fresh Debian deployment on an Android 16 emulator
with 16 KiB host pages. [Core checks](emulator16k/kernel-core.txt) confirm stable
7.2.9-goblin, ext4, sudo/login, hard links, a sparse 10 GiB file, pipelines,
80 concurrent processes and package consistency.

[Package installation](emulator16k/test-packages.txt) verified signed repository
access and installation/execution of Python, less, Vim and hello.

The [release matrix](emulator16k/matrix/result.json) passed same-identity signed
APK replacement with data/configuration/package preservation, binary transport,
IPv4/IPv6 TCP/UDP services, live port changes and conflicts, 260 listener rules,
UDP/TCP DNS, user services, backup/restore, retained-disk recovery, rescue,
30 seconds of native multicore stress, wake-lock transitions, screen-off work,
shutdown/restart, and full terminal acceptance including Unicode, editors,
input, zoom, activity recreation and multiple terminals.

## Scope

The 0.3.5 kernel source and multicore validation continues to apply to the
identical kernel binary. This preview does not repeat physical Samsung,
Android 12, or unplugged battery testing. Android 13–15 and other physical
manufacturers still require direct coverage. Android 10–11 remain experimental.
This is a sideload preview; no Google Play approval is claimed.
