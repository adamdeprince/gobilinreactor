# Acceptance measurements, 2026-09-18

APK SHA256: `55cb1a2d1733b770a2236e02e9b9f9a6b2700d757880ed5c203e7c71da1d40d2`

All six suites passed on both targets. Values below are measured in the developer and recovery suites, without initializing the interactive kitty runtime.

| Target | Page bytes | Median warm startup ms | getpid wall µs/call | Fork/wait ms/cycle | zlib build/test s | Max sampled broker RSS MiB |
|---|---:|---:|---:|---:|---:|---:|
| SM-S928U | 4096 | 329.94 | 3.71 | 50.83 | 21.57 | 207.58 |
| sdk_gphone16k_arm64 | 16384 | 76.51 | 31.82 | 3.76 | 6.33 | 192.84 |

Startup includes mount/recovery, root scanning, DNS and ELF setup. The syscall benchmark executes 200,000 getpid calls. The fork figure is (total minus startup) / 32, including child execution, waits and private-data checks. RSS is sampled roughly every 50 ms and can miss transient peaks; it excludes guest child RSS. Phone/emulator results are separate baselines, not a controlled page-size comparison.

The complete records are in [matrix.json](matrix.json). Definitions and compatibility limits are in [testing.md](../../../docs/testing.md).
