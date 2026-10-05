# GoblinReactor 0.3.3 release validation

October 5, 2026. Version code 30003, ARM64, existing UML kernel build #17.

This release changes branding and license/source distribution. It retains the
application ID and production signing identity. It adds readable notices to the
app, packages the original application license and provides matching source
archives, build instructions and release checksums.

## Artifact and source checks

- [APK checks](release-artifact.json): version 0.3.3, exact GoblinReactor labels,
  matching embedded product metadata, project/Debian/boot notices, production
  v3 signing, non-debuggable packaging, no test components or signing secrets,
  and 16 KiB ZIP alignment.
- [App Bundle checks](bundle-artifact.json): signed bundle and generated splits,
  payload equality with the APK, native extraction and 16 KiB ELF/ZIP alignment.
- [Runtime comparison](runtime-comparison.json): all 94 existing native and
  runtime/deployment asset files match the previously validated 0.3.2 APK.
  New notices and release metadata are additional assets.
- [Kernel source check](kernel-source-check.txt): all 29 files changed by the
  kernel patch stack match the original compiled source tree. Patches apply
  cleanly to pinned source and are idempotent. Kernel configuration matches the
  original builder's configuration.
- The boot-package audit verifies the hashes of every packaged third-party
  boot file against the original Debian builder, then records binary/source
  versions and the statically linked C/C++ runtimes.
- Source collection verifies 237 pinned downloads plus the UML source exports.
  It covers 63 Debian source package/version pairs and upstream terminal/Python
  sources, Android recipes and local patches. The passt Git bundle was cloned
  locally and its Android patcher ran successfully without an upstream fetch.
- The source-cache restoration command succeeds using the collected sources.
  The final handoff verifier checks every source member, every distribution
  checksum, source/APK identity and exact notice equality with the APK.

Artifact hashes are in the linked JSON reports and in the handoff SHA256SUMS.
The handoff SOURCE-REVISION identifies the release source commit. The source
archive is built from that commit and checked against the APK's embedded hashes
of build inputs. Private signing keys and build VM disks are excluded.

## Scope

These are artifact and source-distribution checks. No new phone/emulator
acceptance run, full clean rebuild of every third-party dependency, Google Play
upload, or public publication was performed for this release. The prior
[0.3.2 lifecycle tests](../release-0.3.2/README.md) and
[0.3.0 family matrix](../family-beta/README.md) remain historical evidence;
they do not establish device acceptance of this exact APK.

Android 12+ on a 64-bit ARM64 Android installation remains the support target.
Android 13–15, additional physical manufacturers, PIN-protected and physical
reboot behavior, and long-duration battery/background use still need the
launch checks described in the internal testing guide. Android 10–11 remain
experimental. Google Play eligibility remains unresolved.
