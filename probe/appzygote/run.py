#!/usr/bin/env python3
"""Collect a bounded app-zygote experiment without changing Android policy."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("serial")
parser.add_argument("--mode", choices=("zygote", "regular"), default="zygote")
parser.add_argument("--children", type=int, default=64)
parser.add_argument("--seconds", type=int, default=180)
parser.add_argument("--output", type=Path)
args = parser.parse_args()
if args.children < 1 or args.seconds < 1:
    parser.error("children and seconds must be positive")
if args.mode == "regular" and not args.serial.startswith("emulator-"):
    parser.error("the regular-service control intentionally triggers trimming; run it only on an emulator")
adb = os.environ.get("ADB", str(Path.home() / "Library/Android/sdk/platform-tools/adb"))
base = [adb, "-s", args.serial]
output = args.output or Path(__file__).resolve().parent / "results" / f"{args.serial}-{args.mode}-{int(time.time())}"
output.mkdir(parents=True, exist_ok=True)

def shell(*command):
    result = subprocess.run(base + ["shell", *command], text=True, capture_output=True, timeout=25)
    if result.returncode:
        raise RuntimeError(f"{command}: {result.stderr.strip()}")
    return result.stdout

def policy():
    return {
        "monitor_setting": shell("settings", "get", "global", "settings_enable_monitor_phantom_procs").strip(),
        "monitor_property": shell("getprop", "persist.sys.fflag.override.settings_enable_monitor_phantom_procs").strip(),
        "phantom_limit": [line.strip() for line in shell("dumpsys", "activity", "settings").splitlines() if "max_phantom_processes=" in line],
        "selinux": shell("getenforce").strip(),
    }

metadata = {"serial": args.serial, "mode": args.mode, "children": args.children, "seconds": args.seconds,
            "fingerprint": shell("getprop", "ro.build.fingerprint").strip(), "before": policy()}
(output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
command = base + ["shell", "am", "instrument", "-w", "-e", "mode", args.mode,
                  "-e", "children", str(args.children), "-e", "seconds", str(args.seconds),
                  "dev.goblinlinux.zygoteprobe/.Research"]
started = time.monotonic()
samples = []
with (output / "instrumentation.txt").open("w") as log:
    process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, text=True)
    while process.poll() is None:
        elapsed = time.monotonic() - started
        if elapsed > args.seconds + 60:
            shell("am", "force-stop", "dev.goblinlinux.zygoteprobe")
            process.wait(timeout=20)
            raise RuntimeError("probe exceeded its deadline")
        processes = shell("ps", "-A", "-o", "PID,PPID,USER,NAME,CMD")
        relevant = [line for line in processes.splitlines() if "gz-worker" in line or "gz-helper" in line or "zygoteprobe" in line]
        activity = shell("dumpsys", "activity", "processes")
        tracked = [line.strip() for line in activity.splitlines()
                   if "PhantomProcessRecord" in line and re.search(r"zygoteprobe|gz-|libzygotechild", line)]
        sample = {"elapsed": round(elapsed, 1), "workers_alive": sum("gz-worker" in line for line in relevant),
                  "tracked_phantoms": tracked, "processes": relevant}
        samples.append(sample)
        print(f"{args.serial} {args.mode}: elapsed={sample['elapsed']} workers={sample['workers_alive']} tracked={len(tracked)}", flush=True)
        (output / "samples.json").write_text(json.dumps(samples, indent=2) + "\n")
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            pass
metadata["after"] = policy()
metadata["duration_seconds"] = round(time.monotonic() - started, 1)
metadata["adb_returncode"] = process.returncode
text = (output / "instrumentation.txt").read_text()
metadata["workload_passed"] = f"PASS workload children={args.children} seconds={args.seconds}" in text
metadata["tcp_passed"] = "PASS parent TCP verification" in text
metadata["policy_unchanged"] = metadata["before"] == metadata["after"]
(output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
logs = subprocess.run(base + ["logcat", "-d", "-s", "ActivityManager:I"], text=True, capture_output=True, timeout=20).stdout
(output / "activity-log.txt").write_text("\n".join(line for line in logs.splitlines()
    if re.search(r"zygoteprobe|gz-|libzygotechild", line)) + "\n")
print(text, flush=True)
print(f"Evidence: {output}", flush=True)
if args.mode == "zygote" and not (metadata["workload_passed"] and metadata["tcp_passed"] and metadata["policy_unchanged"]):
    raise SystemExit(1)
