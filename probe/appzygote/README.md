# App-zygote feasibility probe

This separate APK tests Android-managed isolated-service hosting for Goblin's UML
backend. It does not contain or modify Goblin, boot Linux, or change Android policy.
See [the research report](../../docs/app-zygote-research.md) for conclusions and
remaining integration work.

Build with the locally installed Android SDK, NDK 29.0.14206865, build tools 36.1.0,
API 36, and JDK 21:

```sh
bash probe/appzygote/build.sh
adb -s emulator-5556 install --no-incremental -r probe/appzygote/build/zygote-probe.apk
python3 probe/appzygote/run.py emulator-5556
```

`run.py` accepts `ADB`, `--children`, `--seconds`, and `--output`. It collects the
monitoring policy, build fingerprint, process snapshots, native report, and
relevant ActivityManager logs. The experiment exits after its requested duration;
worker parent-death signals and deadlines also bound cleanup after a failure.

The `--mode regular` control runs exactly the same native code in an ordinary
service. It intentionally exceeds Android's process-monitor threshold, so run it
on an otherwise idle emulator. The runner rejects that mode on physical-device
serials. Its expected result is a killed helper and ActivityManager's explicit
phantom-process trimming message; a successful regular run would mean the
restriction was not demonstrated during that observation window.

The manifest uses public `isolatedProcess` / `useAppZygote` attributes, public
binding and Binder FD transfer, and JNI. The isolated service has no added
privileges. Instrumentation holds a bounded wake lock and a service binding for
the measurement. No shell permission is adopted inside the test.

Recorded evidence:

| Directory | Outcome |
| --- | --- |
| [emulator4k-zygote](results/emulator4k-zygote/metadata.json) | 64 workers completed 180 seconds; 8 busy workers |
| [emulator16k-zygote](results/emulator16k-zygote/metadata.json) | 64 workers completed 180 seconds; 4 busy workers |
| [emulator4k-regular-control](results/emulator4k-regular-control/metadata.json) | Helper killed after approximately 144 seconds; explicit phantom trimming in activity-log.txt |

The recorded APK SHA-256 is
`b34aae25baaaf99d8647094ab2b81d5bd5e6fa46735f3e70467a0fedc015c68e`.

Collection correction: the first two runs' `samples.json` files used `ps NAME`,
which is the executable name on these devices rather than the `prctl` task name.
Their worker-count field consequently reads zero. Independent snapshots using
`NAME,CMD` in `verified-snapshots.json` show 64 simultaneous workers, and the native
reports verify completion of every child. Phantom-record matching was unaffected.
The runner now requests both columns. `probe-build.json` preserves this note
alongside the original records; the original measurements were not rewritten.

The source audit records branch names and content hashes in
[source-audit.json](results/source-audit.json). Device execution coverage is limited
to these Android 16 emulators; older versions were inspected in source only.

To remove only the research APK after an experiment:

```sh
adb -s emulator-5556 uninstall dev.goblinlinux.zygoteprobe
```
