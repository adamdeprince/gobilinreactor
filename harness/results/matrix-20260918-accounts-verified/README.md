# Accounts and independent-terminal validation

APK SHA256: `376b93a05928d3908c9213b4120ec669b3cf314a9872fb4b45653432ea36efdb`

All six suites passed on both API 36 ARM64 emulators. The 4 KiB emulator started with a fresh Debian root. The 16 KiB emulator reused its installed root, checking account/data preservation.

| Target | Page bytes | Core checks | VFS/ELF checks | Terminal checks | Lifecycle checks |
|---|---:|---:|---:|---:|---:|
| sdk_gphone64_arm64 (emulator-5558) | 4096 | 99 | 49 | 15 | 10 |
| sdk_gphone16k_arm64 (emulator-5556) | 16384 | 99 | 49 | 15 | 10 |

The complete results, measurements and APK identity are in [validation.json](validation.json). Kernel, package, developer, recovery and terminal results come from the initial [matrix.json](matrix.json). Only lifecycle needed a test-runner correction and rerun on the same APK: Android discarded stale events from a long synthetic key batch, so input is now sent in 32-character batches. Each device directory retains the initial failure plus `lifecycle-final.txt` and screenshots. No application checks were skipped.

The new native time-query path also passed with deliberately slow broker scheduling; see [that regression](../clock-stress-20260918/README.md). The same final APK is now installed on the Samsung SM-S928U and passed its kernel, terminal and lifecycle suites; see the separate [phone validation](../phone-20260918-accounts-final/README.md). Package, developer and recovery suites passed on the earlier phone account build; both emulators above cover those suites on the final APK.
