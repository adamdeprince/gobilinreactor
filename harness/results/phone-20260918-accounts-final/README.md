# Final phone accounts and terminal validation

The final account/runtime APK is installed on the Samsung SM-S928U (`R5CX3468YQR`, API 36, 4096-byte pages). Installation preserved the existing Debian root. The installed APK's checksum matches the local build's SHA256:

`376b93a05928d3908c9213b4120ec669b3cf314a9872fb4b45653432ea36efdb`

| Suite | Result | Evidence |
|---|---|---|
| Kernel regression | PASS: 99 core checks, 49 VFS/ELF checks and guest execution scenarios, including credentials and native time queries | [Report](regression.txt) |
| Native terminal | PASS: 15 checks | [Report](kitty.txt), [screenshot](kitty.png) |
| Android lifecycle | PASS: 10 checks | [Report](lifecycle.txt), [screenshot](lifecycle.png) |

[validation.json](validation.json) records the build identity, device details, exit statuses and lifecycle-runner checksum. Android instrumentation reports `INSTRUMENTATION_CODE: -1` for its successful `Activity.RESULT_OK`; the terminal verdict is `KITTY ACCEPTANCE PASS`.

The terminal checks cover the unprivileged `goblin` account and `/home/goblin`, separate account homes, direct Unicode input, less and Vim, white foreground on black, extra keys, independent shells, explicit root administration, and a detached HTTP server surviving all terminals closing. Lifecycle checks cover actual Android key input, reconnecting to a live shell, closing and reopening terminals around a detached job, retained files/packages after app restart, and shell exit closing the activity. Goblin was [reopened after the tests](ready.png).

These are the three suites rerun on the phone's final APK. Package, developer and recovery suites passed on its earlier account build; all six suites passed on this exact APK on both [4 KiB and 16 KiB emulators](../matrix-20260918-accounts-verified/README.md).

Closing terminals preserves Linux and detached services. Android force-stop or death of the shared app process still terminates running Linux processes; installed files persist.
