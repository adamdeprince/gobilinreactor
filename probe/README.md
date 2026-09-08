# Phase 0: device capability probe

Every load-bearing claim in this project's design is an empirical question about a
particular device's kernel configuration and SELinux policy. This probe answers them
before a line of the sentry gets written.

```sh
./probe/build.sh      # no Gradle, no AGP, no network
./probe/run.sh        # install, run, collect; optional device serial as $1
```

Results land in `probe/results/`. Commit them — the design has to hold across a
range of devices, not just one.

## Why it is shaped like this

**It must run as `untrusted_app`.** Results gathered from `adb shell` are worthless:
the `shell` SELinux domain has entirely different rules, and the restrictions this
project routes around simply do not apply there.

**It must target a modern SDK.** The `execve`-from-data-directory restriction begins
at API 29. The probe is built `targetSdkVersion 36` precisely so it lives on the far
side of it.

**It has no Java and no `classes.dex`.** `android:hasCode="false"` plus the
framework's `NativeActivity` host. That is not a stunt — it means the probe measures
exactly the C++ environment the sentry will run in.

**A real device, not the emulator.** Emulator kernels differ from shipping ones in
exactly the areas being measured: memfd execute permission, `userfaultfd` availability,
page size, and the vendor's SELinux additions.

## What each result decides

| Probe | If it fails |
|---|---|
| `anon mmap RWX` / `anon RW -> mprotect RX` | No way to place guest code in memory. **The project does not exist.** |
| `SVC from JIT page trapped` | Guest syscalls cannot be intercepted. **The project does not exist.** |
| `return value injectable` | Syscalls can be seen but not emulated. **The project does not exist.** |
| `filter stayed thread-local` | The sentry cannot make syscalls of its own; needs a separate process per sentry. |
| `trap round-trip` | Sets the performance ceiling for syscall-heavy workloads. gVisor's systrap lands in the low microseconds. |
| `memfd mmap R+X` | Every guest process pays full RAM for its own copy of libc. Costly, not fatal. |
| `fork() off a native thread` | One-Android-process-per-guest-address-space is unavailable; needs a different task backend. |
| `process_vm_readv` | The sentry must bounce all guest memory access through the guest itself. |
| `userfaultfd` | `fork()` copies eagerly instead of lazily. Slower, not fatal. |

The entries marked *expected to fail* — `execve app-data file`, `app-data file mmap
PROT_EXEC`, `dlopen from app data`, `unshare(CLONE_NEWUSER)` — are the constraints
being designed around. Their failure is the confirmation. If one of them unexpectedly
*succeeds* on some device, the loader has a cheaper path available there.
