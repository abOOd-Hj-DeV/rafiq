#!/usr/bin/env python3
"""Explicit presentation mode: VPU red detection, stop only dev.k230.red_demo."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import selectors
import shlex
import signal
import subprocess
import sys
import time

from k230_dual_linux_service import (
    ROOT, clear_stopped_rings, little, stop_inspector, stop_little_bridge,
)

TARGET = "dev.k230.red_demo"
PID = Path("/var/run/rafiq-red-demo.pid")
STOPPING = False


def stop_requested(_signal, _frame):
    global STOPPING
    STOPPING = True


def eligible_event(event, now_us):
    return (isinstance(event, dict) and event.get("event") == "red"
            and type(event.get("pts_us")) is int and event["pts_us"] >= 0
            and type(event.get("red_fraction")) in (float, int)
            and 0.80 <= event["red_fraction"] <= 1.0
            and type(event.get("detected_at_us")) is int
            and 0 <= now_us - event["detected_at_us"] <= 500_000)


def run(phone):
    sys.path.insert(0, "/root/rafiq-little-linux")
    child = None
    with open(ROOT / "service.lock", "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        PID.write_text(str(os.getpid()))
        signal.signal(signal.SIGTERM, stop_requested)
        signal.signal(signal.SIGINT, stop_requested)
        try:
            devices = little("adb devices")
            authorized = [line.split()[0] for line in devices.splitlines()
                          if len(line.split()) == 2 and line.split()[1] == "device"]
            if authorized != [phone]:
                raise RuntimeError("expected only the explicitly selected authorized phone")
            adb = shlex.join(["adb", "-s", phone, "shell"])
            if not little(adb + " pm path " + TARGET).strip().startswith("package:"):
                raise RuntimeError("install the red demo APK first")
            if not little(adb + " pm path dev.k230.mentor_app").strip().startswith("package:"):
                raise RuntimeError("Mentor is not installed")
            enabled = little(adb + " settings get secure enabled_accessibility_services")
            if "dev.k230.mentor_app/" not in enabled or "LayoutService" not in enabled:
                raise RuntimeError("enable Mentor Accessibility in Android settings first")
            stop_little_bridge()
            clear_stopped_rings()
            with open(ROOT / "red-demo-vpu.log", "ab", buffering=0) as errors:
                child = subprocess.Popen(
                    [str(ROOT / "bin/k230-red-demo-detector")],
                    stdout=subprocess.PIPE, stderr=errors, start_new_session=True,
                )
            bridge = [str(ROOT / "bin/k230-bridge"), "--sink", "shmem", "--adb", "adb",
                      "--serial", phone, "--server-jar", str(ROOT / "bin/scrcpy-server"),
                      "--max-size", "640", "--max-fps", "10", "--bitrate", "2000000",
                      "--no-audio", "--diagnostic-only", "--verbose"]
            little("trap '' HUP; " + shlex.join(bridge)
                   + " </dev/null >/tmp/rafiq-bridge.log 2>&1 & echo $! >/tmp/rafiq-bridge.pid")
            print("DEMO ONLY: red-screen test; target=" + TARGET, flush=True)
            next_health_check = time.monotonic() + 3
            with selectors.DefaultSelector() as selector:
                selector.register(child.stdout, selectors.EVENT_READ)
                while not STOPPING:
                    if child.poll() is not None:
                        raise RuntimeError("hardware detector stopped; see red-demo-vpu.log")
                    if time.monotonic() >= next_health_check:
                        state = little("p=$(cat /tmp/rafiq-bridge.pid); "
                                       "if test -r /proc/$p/stat; then "
                                       "awk '{print $3}' /proc/$p/stat; else echo dead; fi")
                        if state.strip() in ("dead", "Z"):
                            raise RuntimeError("capture bridge disconnected")
                        next_health_check = time.monotonic() + 3
                    for key, _ in selector.select(timeout=0.2):
                        line = key.fileobj.readline()
                        if not line:
                            raise RuntimeError("hardware detector disconnected")
                        event = json.loads(line)
                        print(json.dumps(event), flush=True)
                        if eligible_event(event, time.monotonic_ns() // 1000):
                            start = time.monotonic()
                            command = [str(ROOT / "bin/k230-mvp-force-stop-probe"), "adb", phone, TARGET]
                            result = little(shlex.join(command), 40)
                            if "stopped=1 intervention_started=1 error=" not in result:
                                raise RuntimeError("unconfirmed response: " + result)
                            print(json.dumps({"event": "adb_intervention_ack", "target": TARGET,
                                              "after_detection_ms": round((time.monotonic() - start) * 1000),
                                              "result": result.strip()}), flush=True)
        finally:
            stop_inspector(child)
            stop_little_bridge()
            PID.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--daemon", action="store_true")
    mode.add_argument("--stop", action="store_true")
    parser.add_argument("--phone")
    args = parser.parse_args()
    if args.stop:
        if PID.exists():
            pid = int(PID.read_text())
            cmdline = Path(f"/proc/{pid}/cmdline")
            if cmdline.exists() and b"k230_red_demo.py" in cmdline.read_bytes():
                os.kill(pid, signal.SIGTERM)
        return
    if not args.phone or not all(c.isalnum() or c in "-_.:" for c in args.phone):
        parser.error("--phone must be the authorized phone serial")
    if args.daemon:
        if os.fork():
            return
        os.setsid()
        if os.fork():
            os._exit(0)
        with open("/dev/null", "rb") as source, open(ROOT / "red-demo.log", "ab", buffering=0) as log:
            os.dup2(source.fileno(), 0)
            os.dup2(log.fileno(), 1)
            os.dup2(log.fileno(), 2)
        os.closerange(3, 4096)
    try:
        run(args.phone)
    except Exception as error:
        print("demo unavailable: " + str(error), file=sys.stderr, flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
