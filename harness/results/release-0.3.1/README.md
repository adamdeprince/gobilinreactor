# GoblinReactor Linux® Environment 0.3.1 validation

2026-09-20. Version code 30001, ARM64, UML kernel build #17.

- APK SHA-256: `ca3956bb0af34abfa947ee603d3526bd6f625cb70151eddd16d1271fb003cc26`.
- App Bundle SHA-256: `0f9ca8f69990d32850ca51317eaefc1a81da2c37ca196e60e1105f10e3ab0234`.
- Baseline 0.3.0 APK SHA-256: `d1a2f3d68c1ca78363464a67a17ba1e55a6ede7bb1fc5f44c220c158bd8cd119`.
- Kernel SHA-256: `c02a09463aa5120127362ec60e5ddbdfad320f34ece5ac5a88fbcd3aa633aa86`.

This release adopts the GoblinReactor product name in Android labels, About,
diagnostics, accessibility text, documentation and handoff filenames. The
application ID `dev.goblinlinux.sentry` and release signing identity are retained.
Guest accounts and deployment settings retain their existing names.

The [payload comparison](runtime-comparison.json) confirms all 94 native
libraries and runtime/deployment assets match the distributed 0.3.0 APK byte
for byte. Release metadata changes only the name and version. The remaining
APK changes are branding strings in its manifest and Java UI code.

The [APK inventory](release-artifact.json) passes production signing,
non-debuggable packaging, exclusion of test components and secrets, and ZIP
alignment. The [App Bundle inventory](bundle-artifact.json) verifies generated
split signatures, native executable extraction, 16 KiB ELF/ZIP alignment and
agreement with the APK's libraries and assets. The App Bundle is prepared for
later Play testing; it has not been uploaded.

The [Android 12 update check](upgrade-smoke.json) installs 0.3.1 over 0.3.0,
retains the guest disk inode and a synced file in `/home/goblin`, and returns
to the `goblin` shell. The [About screen](about.png) displays the new full name
and version; its help text includes trademark attribution. This check used the
direct APK on a disposable emulator, not a new physical-phone or split-install
test. An initial automation wait for the shutdown panel timed out after the
guest stopped; the process list confirmed shutdown before the update continued.

## Compatibility evidence

The unchanged 0.3.0 runtime passed the [family test matrix](../family-beta/README.md)
on the Samsung SM-S928U with Android 16, Android 16 emulators with 4 KiB and
16 KiB pages, and a fresh Android 12 emulator with 2 GiB RAM. Those tests cover
the terminal, guest deployment, package installation, parallel execution,
networking, power handling and storage recovery. They were not repeated for
this naming update, and their recorded APK hashes still refer to 0.3.0.

Android 12 or newer with a 64-bit ARM64 Android installation remains the
support target. Android 10–11, Android 13–15, other physical manufacturers,
long-term battery behavior and a week of family use remain outside the direct
coverage recorded here.

The product-name sublicense still needs application and approval before public
distribution; see [the naming record](../../../docs/trademark.md).

Linux® is the registered trademark of Linus Torvalds in the U.S. and other countries.
