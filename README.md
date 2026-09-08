# goblin-linux

A Linux environment for Android that runs **native aarch64 code at full speed**, runs
**stock Debian arm64 packages**, and is **distributable on Google Play**.

Termux gets two of those three. It ships a bootstrap of native binaries and `execve()`s
them, which forces `targetSdkVersion 28` (Android blocks executing files out of an app's
data directory from API 29 on) and puts it outside Google Play's Device and Network Abuse
policy. goblin-linux takes the third option: the guest never asks the Android kernel to
execute anything.

## How

A **userspace kernel** in the shape of [gVisor's systrap platform](https://gvisor.dev/blog/2023/04/28/systrap-release/).

- Guest code is real aarch64, executing natively on the CPU. No emulator, no interpreter,
  no instruction translation.
- It is loaded by *our own* ELF loader into anonymous `PROT_EXEC` memory — the same
  primitive every JIT uses — so `execve()` on a guest file never happens and the API 29
  W^X restriction is never hit.
- A restrictive seccomp filter turns every guest syscall into `SIGSYS`. Our in-process
  kernel services it. The guest has no route to binder, to JNI, or to the Android
  filesystem outside its sandbox.

That last point is also the compliance argument, and it is a much stronger one than
Termux ever had. See [docs/compliance.md](docs/compliance.md).

## Shape of the thing

| Piece | What it is |
|---|---|
| `sentry/` | The userspace kernel. Linux syscall ABI, VFS, procfs, pipes, ptys, signals, futex, sockets. |
| `loader/` | ELF loader + guest dynamic-linker support, mapping into anonymous exec memory. |
| `term/` | kitty, ported to Android: `glfw-android` backend, Android font backend, GLES shaders, embedded CPython. |
| `app/` | The Android app. C++ throughout, JNI only where the NDK has no equivalent. |
| `keys/` | Termux-style extra-keys row and IME handling. |
| `wayland/` | Framebuffer-backed Wayland output, toggled from inside the guest by a CLI tool. |
| `probe/` | **Phase 0.** Device capability probe. Gates every design decision below it. |

## Status

Phase 0. Nothing above `probe/` is written yet, because whether the design is possible
at all is an empirical question about a specific device's kernel and SELinux policy.
Run the probe first: [probe/README.md](probe/README.md).

## Decisions on record

- **Android only.** iOS cannot execute unsigned code under any entitlement available to a
  normal App Store app, so "native CPU" and "stock distro packages" cannot both hold there.
- **Debian arm64, glibc.** No musl bring-up phase.
- **kitty proper**, not a reimplementation. GPLv3; fine on Google Play.
- **C++**, not Java/Kotlin.
