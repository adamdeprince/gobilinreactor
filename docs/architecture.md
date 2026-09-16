# Architecture

## The constraint that shapes everything

Two rules, and every design decision falls out of them.

**Android, from API 29 on, will not `execve()` a file in an app's data directory.**
This is enforced by SELinux, not by policy text. It is why Termux is pinned to
`targetSdkVersion 28`: its whole model is a bootstrap of native binaries in
`$PREFIX/bin` that it executes directly. Google Play requires a recent target SDK,
so that model and the Play Store are mutually exclusive.

**Google Play forbids downloading executable code from a source other than Play** —
and exempts "code that runs in a virtual machine or an interpreter where either
provides indirect access to Android APIs." Downloading a Debian rootfs and executing
it natively is the prohibited thing. Running it inside a sandbox that mediates every
syscall is the exempted thing. See [compliance.md](compliance.md).

So the guest must never be handed to the Android kernel for execution, and it must
never reach an Android API. Both are satisfied by the same mechanism.

## Why not the obvious alternatives

| Approach | Why not |
|---|---|
| PRoot / chroot-style, as Termux does | `execve()`s guest binaries. Dead at API 29. |
| Full-system emulation (QEMU TCG, TinyEMU) | Compliant and safe, but 10–30x slower and a slow kernel boot on every launch. Rejected: the point is native speed. |
| Android Virtualization Framework | `MANAGE_VIRTUAL_MACHINE` is a privileged permission. Not available to normal apps, and device support is narrow. |
| Bundle prebuilt binaries in `jniLibs` | The one directory Android *will* execute from — but nothing new can ever be installed, which forfeits `apt` entirely. |

## The design

A **userspace kernel**, in the shape of [gVisor's systrap platform](https://gvisor.dev/blog/2023/04/28/systrap-release/).

```
┌─ Android app process ─────────────────┐   ┌─ stub process (one per guest addr space) ─┐
│                                       │   │                                            │
│  kitty  ──pty──┐                      │   │   guest code: real aarch64 Debian arm64    │
│                │                      │   │   binaries, executing natively on the CPU  │
│  sentry ◄──────┘                      │   │                    │                       │
│   VFS, procfs, pipes, ptys, signals,  │   │                    │ svc #0                 │
│   futex, sockets, process table       │   │                    ▼                       │
│                     ▲                 │   │   seccomp SECCOMP_RET_TRAP ──► SIGSYS       │
│                     └── shared mem ───┼───┼── stub handler marshals the syscall         │
└───────────────────────────────────────┘   └────────────────────────────────────────────┘
```

- **Guest code is native aarch64**, executing on the real CPU. No emulator, no
  interpreter, no binary translation.
- **Our own ELF loader** maps it into *anonymous* `PROT_EXEC` memory — the same
  primitive every JIT uses — so `execve()` on a guest file never happens. The guest's
  own dynamic linker is loaded the same way, and its file-backed exec mappings are
  serviced by copying rather than mapping.
- **A restrictive seccomp filter** turns every guest syscall into `SIGSYS`. The stub's
  handler hands it to the sentry over shared memory. The guest has no route to binder,
  to JNI, to Android APIs, or to the filesystem outside its sandbox.

`apt` and `dpkg` work unmodified under this, because they only ever write files.
Nothing needs an Android execute bit.

### Task backend

Guest processes need real, separate address spaces — `fork(2)` is meaningless
otherwise. One Android process per guest address space.

Android only starts processes the manifest declares, so a small pool of
manifest-declared stub services acts as zygotes, and further address spaces come from
raw `fork()` inside them. This is Chrome's renderer model. A forked child of an app
process has an unusable ART runtime, which is fine: stubs touch nothing but raw
syscalls and the shared-memory channel.

`fork()` itself:
- **fork-then-exec**, which is overwhelmingly what shells do, takes a fast path into a
  fresh stub with no memory copy.
- **fork without exec** copies the parent's writable memory. Lazily via `userfaultfd`
  if the device allows it, eagerly if not. The probe measures which.

### Filesystem

The rootfs lives as ordinary files in the app's private directory. Reading and writing
them is unrestricted — only *executing* them is not, and we never do. The sentry's VFS
maps guest `/` onto that directory, synthesises `/proc`, `/sys` and `/dev`, and exposes
user-chosen Android directories through the Storage Access Framework.

## Terminal

kitty, ported — not reimplemented. It already has the seams:

- **`glfw-android`**: kitty vendors GLFW under `glfw/` and builds per-platform backends
  (`glfw-x11.so`, `glfw-wayland.so`). Android becomes another one.
- **Android font backend**: alongside the existing `fontconfig.c` and `core_text.m`.
  FreeType and HarfBuzz get bundled; font enumeration comes from Android.
- **GLES 3.2 shaders**: kitty targets desktop GL 3.3 core. This is the real porting
  work — shader headers and a handful of API differences.
- **Embedded CPython**: kitty's upper layers are Python. CPython has had official
  Android support since 3.13, so they run as-is.

kitty is GPLv3. Fine on Google Play; it is one of the reasons iOS is out of scope.

## Keyboard

Termux's model, because it is the one that works: an extra-keys row for the keys a
soft keyboard does not have (Esc, Ctrl, Alt, Tab, arrows, Fn combinations), modifier
latching, and an `InputConnection` that reports keys rather than trying to be a text
field. Reached from C++ over JNI — the IME APIs have no NDK equivalent.

## Wayland on a framebuffer

The guest runs a real Wayland compositor. The sentry presents it with a display it can
drive without DRM/KMS:

- A **custom wlroots backend** exposing a shared buffer as an output, so any
  wlroots compositor (sway, labwc, cage) works unmodified. `/dev/fb0` is offered as a
  fallback for software that wants a raw framebuffer.
- The buffer is the same memory the app hands to its `ANativeWindow`, so presenting a
  frame is a flip, not a copy.
- Input arrives as Android touch and key events and is injected at the seat, avoiding
  a libinput/evdev dependency.
- GPU acceleration is later work: a gfxstream or virgl bridge onto Android's GLES.

Mode switching is a guest-side CLI tool:

```sh
goblin display on      # app swaps the surface from kitty to the compositor output
goblin display off     # or just exit the compositor
```

It writes to a control socket the sentry owns. When the compositor exits, the surface
goes back to the terminal.

## Rootfs delivery

The Debian arm64 base image ships as a **Google Play asset pack**, so the default
environment is literally delivered by Google Play rather than downloaded from a third
party. `apt` then adds to it inside the sandbox.

## Phases

0. **Capability probe.** Establish that the primitives exist on real devices. → `probe/`
1. **Loader + sentry skeleton.** Load a static binary into anon exec memory, trap its
   syscalls, service enough of them to reach `write(1, "hello", 5)` and `exit`.
2. **Enough kernel for a shell.** VFS, process table, `fork`/`execve`, pipes, signals,
   ptys, futex. Target: interactive `bash` from a Debian arm64 rootfs.
3. **Terminal.** kitty on Android against the sentry's pty.
4. **Package management.** `apt install` end to end.
5. **Keyboard.** Termux-style extra keys and IME handling.
6. **Wayland.** Compositor, framebuffer output, `goblin display`.
7. **Store submission.** Asset pack packaging, data safety, policy declarations.

## What the probe has established

Confirmed on real hardware: **Galaxy S24 Ultra (SM-S928U)**, Android 16 / API 36,
kernel 6.1.145, 4 KiB pages, under Samsung's own SELinux policy — not just AOSP's
emulator image. Every result below matches what the emulator reported.

| Question | Answer |
|---|---|
| Can guest code be placed in executable memory? | Yes, both `PROT_EXEC` anonymous mapping and RW→RX `mprotect`. |
| Are guest syscalls interceptable? | Yes. `SIGSYS` fires, `si_syscall` and the argument registers read correctly, writing `x0` injects a return value, and an `svc` issued from a JIT page traps. |
| What does a trap round-trip cost? | **1.5 us** bare, **3.3 us** for a full guest syscall. Of that, 64 ns is the sentry's own dispatch; the rest is signal delivery. |
| Does the filter stay thread-local? | Yes. The sentry can share a process with its stubs. |
| Is `memfd` execution permitted? | **Yes — including sealed.** See below; this is the best answer available. |
| Is `userfaultfd` available? | No, `EPERM`. `fork()` without `exec` copies eagerly. |
| Can we `fork()` off a native thread? | Yes, and the child reports back over a socketpair. |
| `process_vm_readv`? | Works across our own processes. |
| Address space for guest layouts? | 256 GiB `PROT_NONE` reservations, `MAP_FIXED_NOREPLACE` honoured. |
| Inherited seccomp state? | `Seccomp: 2` — the zygote's filter is already installed and ours stacks on it. |

### The loader design this settles

Sealed `memfd` execution working is the good outcome. The loader can:

1. read a guest ELF's segments out of the rootfs as ordinary file data,
2. write them into a `memfd`,
3. seal it with `F_SEAL_WRITE`,
4. map it `PROT_READ | PROT_EXEC` and share that same `memfd` across every guest
   process that needs the object.

That gets a shared page cache — one copy of libc for all guest processes rather than
one per process — while never asking the kernel to execute a guest *file*, and while
the mapping is provably not self-modifying code, because the backing memory is sealed
read-only before it is ever made executable. It is simultaneously the cheapest option
and the strongest version of the compliance argument.

### One result to be careful about

`mmap(PROT_EXEC)` and `dlopen()` on files in the app data directory were **permitted**;
only `execve` was denied. That is consistent with AOSP policy granting `execute` on
`app_data_file` while withholding `execute_no_trans`, and it means a loader *could*
map guest binaries file-backed and skip the copy entirely.

The project will not do that. Mapping downloaded guest binaries directly as executable
is precisely the pattern Play's policy is aimed at, and doing it would trade the
virtual-machine argument for a marginal memory saving that the `memfd` path already
provides. The behaviour is recorded because it may differ across devices, not because
it is a route worth taking.

### Still unknown

- **16 KiB page devices.** Both machines tested run 4 KiB kernels. Android 15+ hardware
  ships 16 KiB pages, and Debian arm64 ELF segments are aligned for 64 KiB, so guest
  segment placement needs checking there. The loader copies into its own backing store
  rather than mapping the guest file, so file-offset alignment is already a non-issue.
- **Other vendors.** One Samsung device is not every device. The `memfd` execution
  result still needs a runtime check and the anonymous fallback it already has.

### Where the performance lever is

The sentry's dispatch is 64 ns; signal delivery is ~3.2 us. Nothing in the syscall
handler is worth optimising. If guest syscalls ever need to be faster, the change is
architectural and well-trodden: stop returning through the signal path, and have the
stub hand syscalls to a dedicated sentry thread over shared memory, which is what
gVisor's systrap does.
