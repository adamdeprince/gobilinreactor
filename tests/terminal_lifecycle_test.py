#!/usr/bin/env python3
"""Exercise the installed development terminal through Android input events.

Requires a bootstrapped root (run harness/run.sh SERIAL persistent first).
Writes only its own uniquely named guest test directory and temporary UI dump.
"""
import argparse
from pathlib import Path
import re
import shlex
import subprocess
import time
import uuid
import xml.etree.ElementTree as ET
from uml_control import request

PKG = "dev.goblinreactor.sentry"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("serial")
parser.add_argument("--screenshot", type=Path)
args = parser.parse_args()
adb = ["adb", "-s", args.serial]


def call(*command):
    return subprocess.check_output(adb + list(command), timeout=30).decode().strip()


def wait_for(condition, name, seconds=25):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if condition():
            print("PASS:", name, flush=True)
            return
        time.sleep(0.25)
    raise AssertionError(name)


def ui():
    call("shell", "uiautomator", "dump", "/data/local/tmp/goblin-terminal.xml")
    return ET.fromstring(call("shell", "cat", "/data/local/tmp/goblin-terminal.xml"))


def tap(node):
    left, top, right, bottom = map(int, re.findall(r"\d+", node.attrib["bounds"]))
    call("shell", "input", "tap", str((left + right) // 2), str((top + bottom) // 2))


def tap_menu_item(label):
    # Landscape with an open IME can show only a few rows of the popup.
    # Scroll its list instead of assuming every menu item fits on screen.
    for _ in range(12):
        screen = ui()
        item = next((n for n in screen.iter("node") if n.attrib.get("text") == label), None)
        if item is not None:
            tap(item)
            return
        menu = next((n for n in screen.iter("node")
                     if n.attrib.get("class") == "android.widget.ListView"
                     and n.attrib.get("scrollable") == "true"), None)
        if menu is None:
            break
        left, top, right, bottom = map(int, re.findall(r"\d+", menu.attrib["bounds"]))
        x, inset = (left + right) // 2, (bottom - top) // 5
        call("shell", "input", "swipe", str(x), str(bottom - inset), str(x), str(top + inset), "300")
    raise AssertionError("menu item not reachable: " + label)


def command(text):
    view = next(n for n in ui().iter("node") if n.attrib.get("class") == "android.view.SurfaceView")
    tap(view)
    # Wait for the editor/IME layout before injecting hardware events.
    ui()
    # adb shell joins its arguments. Quote for the remote shell; input text
    # handles %s as a space and types shell metacharacters as literal key events.
    # Android timestamps the entire input-text batch at once and drops events
    # once they become stale. Small batches keep long commands valid on a busy
    # emulator while still exercising actual hardware-key dispatch.
    for offset in range(0, len(text), 32):
        call("shell", "input", "text", shlex.quote(text[offset:offset + 32].replace(" ", "%s")))
    call("shell", "input", "keyevent", "66")


def read_guest(path):
    result = request(adb, 'read', path, timeout=15)
    return result.stdout.decode().strip() if result.returncode == 0 else None


def processes():
    rows = call("shell", "ps", "-A", "-o", "PID,PPID,NAME").splitlines()[1:]
    return [(int(p), int(parent), name) for p, parent, name in (r.split(None, 2) for r in rows)]


def app_pid():
    rows = processes()
    ids = {pid for pid, _, name in rows if name == PKG}
    return next(pid for pid, parent, name in rows if name == PKG and parent not in ids)


root = "/home/goblin/goblin-ui-" + uuid.uuid4().hex[:12]
call("shell", "am", "force-stop", PKG)
call("shell", "am", "start", "--activity-clear-top", "-n", PKG + "/.TerminalActivity")
wait_for(lambda: any("goblin@goblin:~$" in n.attrib.get("content-desc", "") for n in ui().iter("node")), "terminal activity starts Bash")
pid = app_pid()
command(f"mkdir {root}; echo $$ > {root}/shell; GOBLIN_SESSION_PROOF=alive; nohup sleep 300 </dev/null >{root}/job-log 2>&1 & echo $! > {root}/job; disown; echo persistent > {root}/data; sync")
wait_for(lambda: read_guest(root + "/data") == "persistent", "typed command reaches guest filesystem")
call("shell", "input", "keyevent", "3")  # Home detaches the activity.
call("shell", "am", "start", "--activity-clear-top", "-n", PKG + "/.TerminalActivity")
command(f"kill -0 $(cat {root}/job) && echo $GOBLIN_SESSION_PROOF > {root}/reconnect")
wait_for(lambda: read_guest(root + "/reconnect") == "alive", "recreated activity reconnects to the same shell and live job")
assert app_pid() == pid
screen = ui()  # Wait for layout/IME animations before capturing or tapping.
if args.screenshot:
    args.screenshot.parent.mkdir(parents=True, exist_ok=True)
    args.screenshot.write_bytes(subprocess.check_output(adb + ["exec-out", "screencap", "-p"], timeout=15))
tap(next(n for n in screen.iter("node") if n.attrib.get("content-desc") == "Terminal menu"))
tap_menu_item("Close terminal")
wait_for(lambda: request(adb, 'exec', f'! kill -0 $(cat {root}/shell) 2>/dev/null && kill -0 $(cat {root}/job)').returncode == 0, "closing the terminal reaps its shell and keeps its detached job")
call("shell", "am", "start", "-n", PKG + "/.TerminalActivity")
wait_for(lambda: any("goblin@goblin:~$" in n.attrib.get("content-desc", "") for n in ui().iter("node")), "new terminal joins the existing Linux runtime")
command(f"kill -0 $(cat {root}/job) && kill $(cat {root}/job) && echo survived > {root}/closed-proof")
wait_for(lambda: read_guest(root + "/closed-proof") == "survived", "detached job survives closing every terminal")
call("shell", "am", "force-stop", PKG)
call("shell", "am", "start", "-n", PKG + "/.TerminalActivity")
wait_for(lambda: any("goblin@goblin:~$" in n.attrib.get("content-desc", "") for n in ui().iter("node")), "terminal starts after app process restart")
command(f"test \"$(cat {root}/data)\" = persistent && python3 --version > {root}/restart")
wait_for(lambda: (read_guest(root + "/restart") or "").startswith("Python 3."), "installed Python and guest files survive app restart")
command(f"rm -r {root}; exit")
pid = app_pid()
wait_for(lambda: request(adb, 'exec', 'test -r /proc/1/status').returncode == 0, "Linux survives the last shell exit")
wait_for(lambda: not any(n.attrib.get("package") == PKG for n in ui().iter("node")), "exit closes the terminal activity")
print("TERMINAL LIFECYCLE PASS", flush=True)
