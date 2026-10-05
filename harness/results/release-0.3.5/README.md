# GoblinReactor 0.3.5 release validation

October 5, 2026. Version code 30005, ARM64, stable Linux 7.2.9-goblin.

The kernel now starts from the checksum-verified official Linux 7.2.9 archive,
with the pinned external ARM64 UML port and the existing Android integration
patches applied separately. The port remains external to upstream Linux.
The Goblin artwork, application ID and release signing identity are retained.

## Source and packaging

- [Kernel build](kernel-build.json) records the upstream archive, port patch,
  kernel binary and build-configuration identities. Source preparation can be
  repeated without changes; the published configuration matches the build.
- [Source checks](kernel-source-check.txt) cover all 223 port and Android
  integration paths, including deletions, and match the successfully built
  source. Android patches apply cleanly and are idempotent.
- [APK checks](release-artifact.json) pass: stable kernel banner matches the
  source pin, production signing, non-debuggable packaging, notices, absence of
  test components/secrets, and 16 KiB ZIP alignment.
- [Bundle checks](bundle-artifact.json) pass: generated splits retain the
  release APK's runtime/assets, signing, native extraction and 16 KiB alignment.
- [Runtime comparison](runtime-comparison.json): only the kernel and four
  source/license metadata assets differ from 0.3.4; 268 runtime/assets match.
- The source/handoff verifier checks the source inventory against the committed
  project and APK build inputs, distribution checksums, exact notice equality
  with the APK, and artifact-report identity.

## Android 16 emulator, 16 KiB host pages

The release APK deployed Debian onto a fresh installation and booted systemd
successfully. The [guest report](emulator16k/kernel.txt) confirms
`7.2.9-goblin`, 16 KiB guest pages and a healthy systemd state.

- [Kernel acceptance](emulator16k/kernel-core.txt): ext4, unprivileged account,
  sudo/login, hard links, a sparse 10 GiB file, pipelines, 80 concurrent
  processes and consistent package state.
- [Package installation](emulator16k/test-packages.txt): signed repositories,
  HTTPS downloads, Python, less, Vim and hello installed and executed.
- [Release matrix](emulator16k/matrix/result.json): same-version signed APK
  replacement preserves data/configuration/packages; binary transport;
  IPv4/IPv6 TCP and UDP forwarding, live port changes, conflicts and 260 rules;
  UDP/TCP DNS; user services; backup/restore, previous-disk recovery and rescue;
  120 seconds of native multicore stress on four CPUs (31 iterations);
  wake-lock transitions, screen-off progress, shutdown/restart; full terminal
  acceptance including Bash, sudo, Unicode, less, Vim, zoom, input, terminals,
  activity recreation and detached services.

## Limits

This release does not repeat the older Android 12 coverage. Android 13–15,
other manufacturers and a long physical unplugged battery run still need
testing. No public upload or Google Play approval is claimed.
