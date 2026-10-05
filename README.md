# GoblinReactor

GoblinReactor is a terminal and local development environment for Android.
It runs a real Linux® kernel built for **User-Mode Linux (UML)** inside an
Android app, with Debian ARM64 packages and the existing native kitty terminal.
The default APK uses UML. The earlier custom Linux ABI implementation in
`sentry/` is retained as historical source and is not linked into the app.

The kernel is based on stable **Linux 7.2.9**, with the ARM64 UML port and Android
integration applied as separate patches. Source checksums and port provenance
are pinned in [uml/sources.lock.json](uml/sources.lock.json).
Guest ARM64 instructions execute natively; Linux implements syscalls, memory,
processes, permissions, filesystems, networking and PTYs. Startup gives Linux
one virtual CPU per configured Android hardware CPU. Threads within one process
can execute concurrently on those CPUs. The guest uses 16 KiB pages on both
4 KiB and 16 KiB Android hosts.

## Terminal and deployment

- Default account `goblin`, home `/home/goblin`, passwordless sudo and an explicit
  root-terminal menu. Accounts and sudo configuration are deployed into Linux.
- Direct terminal input, white foreground on black, pinch resizing, and an
  Esc/Ctrl/Alt/Tab/four-arrow row that hides when a physical keyboard connects.
- Multiple terminals share one running Linux environment. Exiting closes the
  terminal shell; detached servers survive closing every terminal.
- An app-zygote isolated service hosts UML; a separate Android-managed service
  hosts networking. Deployment does not disable Android's phantom-process
  monitoring. See the [device evidence](harness/results/app-zygote/README.md).
- The Linux service holds a partial wake lock while running, including with
  the screen off. **Keep Linux awake** controls it; **Android battery settings…**
  opens Android's battery-optimization exemption settings for unattended use.
- **Start Linux after reboot** is off by default. Enabling it starts the Linux
  foreground service after the first device unlock following a reboot, without
  opening the terminal screen. Enabled Linux services can start again; running
  processes and unsaved work are not restored. Guest data remains in Android's
  credential-encrypted app storage.
- Debian packages and files persist in an ext4 disk image. Existing installations
  migrate automatically, preserving real ownership, permissions and hard links.
  The original Debian directory remains as a migration backup.
- Android app updates replace the UML kernel and runtime while retaining that
  disk. Deployment updates preserve edited or deleted guest configuration;
  unchanged defaults can update automatically.
- The signed `apt.goblinreactor.com` repository is enabled during deployment.
  Purrfect is not preinstalled.

GoblinReactor imposes **no smaller guest RAM, disk, process, terminal, CPU, output or
runtime quota**. Guest RAM uses memory-backed files and the phone's reported
physical-memory capacity. Its sparse disk uses the phone filesystem's capacity.
Linux resource controls are available to guest root. Allocations can exhaust
Android memory or storage; Android's own permissions and memory management still
apply. Guest root does not become Android root.

## Build and run

Build the kernel, network helper and initramfs in a case-sensitive Debian 13
ARM64 Linux environment following [uml/README.md](uml/README.md). The Android
NDK installation and the development host's user configuration are not modified.
Then, on the Android build host:

```sh
python3 fixtures/bootstrap.py
python3 fixtures/deployment.py
python3 terminal/build.py
bash harness/build.sh
bash harness/run.sh DEVICE regression
bash harness/run.sh DEVICE emacs
adb -s DEVICE shell am start -n dev.goblinlinux.sentry/.TerminalActivity
```

`harness/build.sh` builds offline from prepared artifacts and signs the development
APK at `harness/build/goblin-sentry.apk`. Installing it with `adb install -r`
preserves the app's existing Debian data. There is no custom-ABI fallback.

See [architecture](docs/architecture.md), [runtime and storage](docs/persistent-sessions.md),
[UML build details and boundaries](uml/README.md), and [testing](docs/testing.md).
For the signed APK, App Bundle and family test workflow, see [internal testing](docs/internal-testing.md).
Google Play eligibility has not been established; see [distribution notes](docs/compliance.md).
The product name is **GoblinReactor**. See [naming and trademark usage](docs/trademark.md)
for descriptive references to the underlying technology.

Linux® is the registered trademark of Linus Torvalds in the U.S. and other countries.

Original application code is GPL-3.0-or-later, with component-specific terms in
[LICENSE](LICENSE). See [third-party notices](THIRD_PARTY_NOTICES.md) and the
[source distribution instructions](release/README.md).
