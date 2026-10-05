# GoblinReactor 0.3.4 release validation

October 5, 2026. Version code 30004, ARM64, existing UML kernel build #17.

The launcher uses the supplied Goblin character artwork. The source PNG is
retained unchanged at `harness/res/drawable-nodpi/goblin_logo.png`; its SHA-256 is
`6a67da20107c36f4f50bffca06c9397ced97bab54a945343a3b6b8ecf2fdd5f8`.
Adaptive-icon insets place the artwork in the launcher mask's central area,
with a background matching the supplied image. The old terminal glyph is no
longer used for the foreground or themed-icon override. The artwork is covered
by the APK's source-input inventory and included in corresponding source.

## Checks

- [APK checks](release-artifact.json): version, branding, notices, production
  signing, non-debuggable packaging, absence of test components/secrets and
  16 KiB ZIP alignment pass. The packaged icon resource is present and its
  source-input hash matches the supplied image.
- [App Bundle checks](bundle-artifact.json): generated splits retain the APK's
  runtime/assets, signing, native extraction and 16 KiB compatibility.
- [Runtime comparison](runtime-comparison.json): all 272 pre-existing runtime
  and asset files, excluding release metadata, match 0.3.3.
- [Phone update](phone-update.json): an in-place update from 0.3.2 to 0.3.4
  succeeded on the Samsung SM-S928U. Android reports version code 30004, the
  installed APK hash matches the release, and TerminalActivity launched.
  The app was not uninstalled and Android app data was not cleared.
- The final source/handoff verifier checks the full source inventory against
  the source commit and APK build inputs, every distribution checksum, exact
  notice equality with the APK, and artifact-report identity.

This is an icon update with installation/launch verification, not a repeat of
the full device acceptance suite. The dependency/source audit from
[0.3.3](../release-0.3.3/README.md) still applies to the unchanged runtime.
Broader device, lifecycle and battery testing and Google Play review remain
outstanding. No public upload was performed.
