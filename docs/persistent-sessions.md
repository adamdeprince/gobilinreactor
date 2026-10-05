# Persistent environment sessions

The Android foreground service binds an app-zygote isolated service hosting the
real UML kernel and its workers. Networking runs in another Android-managed
service. This avoids the phantom-child count restriction on the tested devices
without changing Android's monitoring settings. Activities display kitty
screens and send terminal bytes; each shell runs on a Linux PTY. There is no
terminal-count cap. Normal shell job control applies: an attached foreground job
receives the PTY hangup, while properly detached jobs can outlive every terminal.
Reopening GoblinReactor joins the same running Linux environment. `exit` closes a shell;
the notification's **Shut down environment** action powers down the environment.

**Keep environment awake** is enabled by default and holds an Android partial wake
lock while the Linux service runs, including when every terminal is closed or
the screen is off. Disabling it leaves Linux running but permits host sleep.
The lock is released at shutdown. **Android battery settings…** lets the user
grant Android's separate Doze exemption for unattended operation.

The default account is `goblin` in `/home/goblin`, with passwordless sudo deployed
inside Debian. The terminal menu can create an account, open as an existing user,
or explicitly open a root terminal. Linux enforces ownership, groups and file
permissions. The Goblin Reactor repository and its scoped key are deployed for
apt installs; Purrfect is not preinstalled.

## Disk and migration

The active filesystem is `files/uml/rootfs.ext4`. It is a sparse ext4 disk image,
not the old host-directory VFS. The read-only exporter converts the previous
ownership xattrs, virtual symbolic links and hard-link representation to ordinary
Linux metadata. The original `files/debian` tree remains a backup and is no longer
the active filesystem. Tests and tools must use the running guest to read files.

A first boot imports into `rootfs.ext4.pending`. After setup succeeds and Linux
syncs its files, the host fsyncs and atomically publishes the image. Interrupted
imports can restart from the unchanged original tree. Later boots run e2fsck;
filesystem growth runs a full check and resize. ext4 journal recovery handles
an abrupt Android app kill. As on a machine losing power, data still only in the
guest's page cache can be lost. Use `sync`/`fsync` when testing durability across
force-stop; normal terminal closure keeps Linux and its cache alive.

## Resource ownership

There is no Goblin RAM window, disk quota, task quota, terminal quota, wall-time
budget or CPU budget. Guest RAM is a memfd sized from the phone's physical-memory
capacity. The sparse disk uses the capacity of the app's backing filesystem;
Android allocates blocks as they are written. Other apps share that storage, so
logical ext4 free space is not a reservation of Android free space. Guest root
can configure Linux limits or consume resources until guest/host exhaustion.
Android's UID permissions, SELinux and memory killer continue to apply.
Startup matches the guest CPU count to Android's configured hardware CPUs;
guest processes and threads can execute on those CPUs concurrently.

For build instructions, diagnostic commands and the current ARM64 port's
boundaries, see [uml/README.md](../uml/README.md). For reproducible checks, see
[testing.md](testing.md).
