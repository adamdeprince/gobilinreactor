# Goblin: Linux guest execution and Google Play policy review

Updated October 5, 2026 for Google Play policy support.
Android package: `dev.goblinreactor.sentry`.
Current proposed public identity: preview 0.3.6, version code 30006.
Earlier implementation evidence below was collected under the private test identity.

**Purpose and requested clarification**

Goblin provides an on-device Debian Linux environment with a terminal, Linux
accounts, package management, development tools, and user-managed services.
It uses a real ARM64 User-Mode Linux (UML) kernel. Users can install and run
native Linux programs through apt, including packages from Debian and
`apt.goblinreactor.com`.

Google Play's [Device and Network Abuse policy](https://support.google.com/googleplay/android-developer/answer/16559646?hl=en)
restricts downloading executable code outside Google Play and describes an
exception for execution in a virtual machine or interpreter that provides
indirect access to Android APIs. We request clarification on whether the
execution model described below qualifies for that exception, and what evidence
Google needs to assess it. This document does not claim an existing approval.

**Execution model**

The Android package bundles the UML kernel, execution stub, native terminal
runtime, network helper, boot tools, and initial Debian deployment data. The
user starts Linux through Goblin's Android interface. Debian systemd, shells,
editors, apt, and installed programs run inside the guest Linux environment.

Downloaded programs include native ARM64 ELF executables and shared libraries.
Their ARM64 instructions execute directly on the CPU in UML execution workers.
UML's seccomp backend intercepts guest system calls and transfers control to the
Linux kernel, which implements guest processes, credentials, filesystems,
networking, and other Linux system-call semantics. This is userspace Linux
virtualization, without a hardware hypervisor or instruction interpreter.
The kernel and execution workers are Android host processes governed by the
host's process and memory management.

Guest root controls the Linux installation, including accounts, configuration,
packages, and apt sources. Users can compile or import programs; execution is
not limited to a fixed package allowlist. These privileges do not grant Android
root. Android UID isolation, SELinux, and the host's resource and lifecycle
controls remain in effect.

**Android boundary and mediated interfaces**

The UML kernel and workers run under `KernelService`, declared with
`android:isolatedProcess="true"` and `android:useAppZygote="true"`.
The service has an isolated UID rather than the ordinary application UID.
The kernel, network, session, and maintenance services are not exported.
The foreground session service binds the kernel host and passes already-open
file descriptors over Binder.

| Guest function | Android integration |
| --- | --- |
| Persistent storage | An app-private sparse ext4 image is opened by the Android host and passed to UML as a block-device descriptor. |
| Boot and deployment data | The host passes an initramfs and read-only boot/seed archive descriptors. There is no live host-filesystem mount into the guest. |
| Terminal input and output | A framed serial transport carries terminal bytes, window sizes, and lifecycle requests between the Android UI and the guest PTY broker. |
| Internet access | Guest packets pass to a separate Android-managed service running passt under the ordinary app UID. That service owns Android-side Internet sockets; the foreground service handles Android DNS requests. |
| Backup and restore | Android's document picker supplies user-selected destinations and sources through the host maintenance flow. |

The guest uses Linux system calls and virtual devices for these functions.
The packaged Android services implement the host integration. Apt does not
install guest packages as Android applications or load them as Java/JNI plugins
in the terminal UI. The isolated-host probe verified that direct opening of
app-private paths and direct creation of Internet sockets failed, while
explicitly passed descriptors remained usable. These are bounded test results,
not a claim that the entire system has undergone an independent security audit.

**Package installation and updates**

Apt downloads and installs Debian-format packages into the persistent guest
filesystem. The default deployment enables Debian and Goblin Reactor sources
with their configured apt signing keyrings. Guest administrators can change
those sources. Package scripts and installed binaries execute inside Linux.
Linux package upgrades modify guest files, configuration, and software.

The existing runtime loads its Android-side native components from the installed
APK and refreshes its boot assets from packaged assets. It retains the existing
guest disk across APK replacements. Apt does not update those Android components
or select a replacement host UML kernel from the guest filesystem.

For the proposed Google Play distribution, the Android app, UML runtime, and
bundled boot assets would be delivered and updated through Google Play.
Release APK and App Bundle packaging have been validated locally. Google Play
distribution has not yet been validated. Guest package installation and upgrade remain
part of the proposed product's functionality.

**Lifecycle and Android controls**

A user-started foreground service owns the Linux instance independently of
terminal windows, allowing guest services to continue after terminals close.
The notification provides shutdown. The app holds a partial wake lock while
Linux runs when the user-controlled "Keep Linux awake" option is enabled; that
option defaults to enabled. A separate menu action can request Android's
battery-optimization exemption.

Starting with version 0.3.2, an optional "Start Linux after reboot" setting
defaults to disabled. When enabled by the user, a non-exported `BOOT_COMPLETED`
receiver starts the same `specialUse` foreground service after the first user
unlock, without opening an activity. The guest disk remains in credential-
encrypted storage. The usual notification, shutdown action and wake-lock
preference apply. Android denial is recorded locally without repeated restart
attempts. This opt-in behavior is included in the manifest's foreground-service
purpose declaration and needs to be reflected in the Play Console declaration.

The implementation uses the public app-zygote service mechanism without changing
device-wide process-monitoring settings or disabling SELinux. Current tests
observe different phantom-process accounting for this hosting arrangement;
that accounting is an Android implementation detail subject to platform or
vendor changes. Android can still terminate the app under memory pressure or
after force-stop. Foreground-service and battery-exemption policy requirements
remain separate review topics from the downloaded-code exception.

**Evidence and reviewer reproduction**

The historical 0.2.1 reference APK was non-debuggable and excluded development
test components. Its SHA-256 is
`a2c0b5ca86361de8827cbc0f7deda84c5e2c74a13a13109c6b1a64dd37c02c14`.
Recorded tests cover Android 16 on a Samsung SM-S928U and 4 KiB/16 KiB emulators,
including guest operation, multiple terminals, networking, APK upgrade
preservation, and recovery. These results establish the tested behavior, not
Google Play eligibility or compatibility with every Android device.

A reviewer can demonstrate package execution by opening a fresh installation,
waiting for the default `goblin` shell, and running:

```sh
whoami
uname -a
sudo apt update
sudo apt install -y hello
hello
dpkg-query -W hello
```

This downloads and executes a Linux package inside the guest. The default
account has passwordless sudo. Goblin's notification shutdown action stops the
Linux instance. A demonstration recording is still to be prepared. The current release
handoff includes a locally validated App Bundle; neither artifact is
represented as attached to this brief.

Implementation and test references in this repository:

- [Architecture](architecture.md) and [UML integration](../uml/README.md).
- [Android service declarations](../harness/AndroidManifest.xml), [Binder descriptor transfer](../harness/java/dev/goblinreactor/sentry/ManagedLinux.java), and [runtime startup and disk handling](../uml/runtime.cpp).
- [Release manifest generation](../harness/manifest.py) and [signed APK inventory](../harness/results/app-zygote/final-release-artifact.json).
- [App-zygote research and isolation observations](app-zygote-research.md) and [integration evidence](../harness/results/app-zygote/README.md).
- [Guest repository configuration](../fixtures/deployment.py).

These relative links refer to supporting repository material; the architectural
description above is self-contained for a support inquiry.

**Question for the policy team**

Does native ARM64 Linux package execution under this UML kernel and isolated
Android hosting model qualify for the Device and Network Abuse policy's
virtual-machine/interpreter exception? In particular, how does Google assess
the requirement for indirect Android API access in this arrangement, and what
additional implementation evidence or reviewer demonstration is needed?

The inquiry can be submitted through [Google Play Console Help and support](https://play.google.com/console/developers/help-and-support)
using the developer account. Google's [advance-notice guidance](https://support.google.com/googleplay/android-developer/answer/6320428?hl=en)
directs questions outside its listed categories to support. This brief has not
been submitted; a support response would not replace review of the release.
