# Implementation status

The earlier ABI-emulation milestones have been superseded by the UML backend.
The default APK now packages a real ARM64 Linux UML kernel, preserves the Android
kitty UI, migrates existing Debian files to ext4, and removes Goblin resource
quotas. `sentry/` remains historical source, not an optional production backend.

Current build instructions, implemented behavior and remaining kernel-port and
network/service-manager boundaries are maintained in [uml/README.md](../uml/README.md).
Acceptance procedures and release evidence are maintained in [testing.md](testing.md).
Old reports under `harness/results` describe the builds named in those reports;
they do not validate a different kernel or APK.
