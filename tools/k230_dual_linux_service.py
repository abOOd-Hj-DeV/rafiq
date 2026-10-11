#!/usr/bin/env python3
"""Supervise the hardware-only Linux/Linux pipeline on K230."""

import argparse
import fcntl
import mmap
import os
from pathlib import Path
import shlex
import signal
import struct
import subprocess
import sys
import time

ROOT = Path("/root/rafiq")
PID = Path("/var/run/rafiq-dual-linux.pid")
STOPPING = False


def stop_requested(_signum, _frame):
    global STOPPING
    STOPPING = True


def daemonize():
    if os.fork():
        return False
    os.setsid()
    if os.fork():
        os._exit(0)
    with open("/dev/null", "rb") as source, open(ROOT / "service.log", "ab", buffering=0) as log:
        os.dup2(source.fileno(), 0)
        os.dup2(log.fileno(), 1)
        os.dup2(log.fileno(), 2)
    os.closerange(3, 4096)
    return True


def stop_service():
    if not PID.exists():
        return
    pid = int(PID.read_text())
    command = Path(f"/proc/{pid}/cmdline")
    if command.exists() and b"k230_dual_linux_service.py" in command.read_bytes():
        os.kill(pid, signal.SIGTERM)


def little(command, timeout=15):
    from little_linux import MAILBOX, execute, preflight

    preflight()
    with open("/var/run/rafiq-little-mailbox.lock", "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
        try:
            with mmap.mmap(fd, 0x100000, offset=MAILBOX) as box:
                if struct.unpack_from("<Q", box, 0x30008)[0] != 0x52414649514C4E58:
                    raise RuntimeError("little Linux is not ready")
                output, status = execute(box, command, timeout)
                if status:
                    raise RuntimeError(f"little command failed ({status}): {output}")
                return output
        finally:
            os.close(fd)


def stop_little_bridge():
    little("if test -s /tmp/rafiq-bridge.pid; then "
           "p=$(cat /tmp/rafiq-bridge.pid); "
           "if test -e /proc/$p/cmdline && grep -q k230-bridge /proc/$p/cmdline; then "
           "kill $p; i=0; while kill -0 $p 2>/dev/null && test $i -lt 6; do "
           "sleep 1; i=$((i+1)); done; "
           "if kill -0 $p 2>/dev/null; then kill -9 $p; fi; fi; "
           "rm -f /tmp/rafiq-bridge.pid; fi", 12)


def clear_stopped_rings():
    for proc in Path("/proc").glob("[0-9]*"):
        try:
            command = (proc / "cmdline").read_bytes().split(b"\0")[0]
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
        if command == str(ROOT / "bin/k230-inspector").encode():
            raise RuntimeError("another inspector owns shared memory; refusing to clear it")
    fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
    try:
        for address in (0x0C000000, 0x0D010000, 0x0D060000, 0x0D100000):
            with mmap.mmap(fd, 4096, offset=address) as ring:
                ring[:64] = bytes(64)
    finally:
        os.close(fd)


def stop_inspector(child):
    if child is None:
        return
    try:
        child.send_signal(signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        child.wait(timeout=30)
    except subprocess.TimeoutExpired:
        os.killpg(child.pid, signal.SIGKILL)
        child.wait(timeout=15)


def supervise(protection_v2=False):
    sys.path.insert(0, "/root/rafiq-little-linux")
    lock = open(ROOT / "service.lock", "a")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print("service is already running", flush=True)
        return
    PID.write_text(str(os.getpid()))
    signal.signal(signal.SIGTERM, stop_requested)
    signal.signal(signal.SIGINT, stop_requested)
    child = None
    backoff = 2
    try:
        while not STOPPING:
            try:
                stop_little_bridge()
                devices = little("adb devices")
                authorized = [line.split()[0] for line in devices.splitlines()
                              if len(line.split()) == 2 and line.split()[1] == "device"]
                if len(authorized) != 1:
                    raise RuntimeError("expected exactly one authorized phone; will retry")
                clear_stopped_rings()
                args = [
                    str(ROOT / "bin/k230-inspector"), "--source", "shmem", "--verdicts", "shmem",
                    "--kmodel", str(ROOT / "nsfwjs-mobilenet-v2.kmodel"),
                    "--ui-kmodel", str(ROOT / "android-ui-yolov8n-logits.kmodel"), "--ui-logits",
                    "--audio-kmodel", str(ROOT / "yamnet-smoke.kmodel"),
                    "--kpu-worker", str(ROOT / "bin/kpu_worker.py"),
                    "--audio-before-ms", "475", "--audio-after-ms", "500", "--max-wait-ms", "1500",
                    "--analysis-fps", "2",
                    "--idle-timeout-ms", "30000", "--verbose",
                ]
                if protection_v2:
                    args.append("--protection-v2")
                else:
                    args.extend(["--mvp-force-stop", "--block", "0.70"])
                env = dict(os.environ, PYTHONPATH=str(ROOT / "python"))
                if (ROOT / "inspector.log").exists():
                    (ROOT / "inspector.log").replace(ROOT / "inspector.previous.log")
                with open(ROOT / "inspector.log", "wb", buffering=0) as log:
                    child = subprocess.Popen(args, env=env, stdout=log, stderr=log, start_new_session=True)
                bridge = [
                    str(ROOT / "bin/k230-bridge"), "--sink", "shmem", "--adb", "adb",
                    "--serial", authorized[0], "--server-jar", str(ROOT / "bin/scrcpy-server"),
                    "--max-size", "640", "--max-fps", "2", "--bitrate", "1000000",
                    "--audio-codec", "raw", "--verbose",
                ]
                bridge.extend(["--protection-v2"] if protection_v2 else ["--diagnostic-only", "--force-stop"])
                little("trap '' HUP; " + shlex.join(bridge) +
                       " </dev/null >/tmp/rafiq-bridge.log 2>&1 & echo $! >/tmp/rafiq-bridge.pid")
                started = time.monotonic()
                mode = "protection-v2" if protection_v2 else "MVP force-stop"
                print(f"started hardware-only {mode} pipeline; phone={authorized[0]}", flush=True)
                while not STOPPING and child.poll() is None:
                    time.sleep(1)
                    if time.monotonic() - started > 10:
                        response = little("p=$(cat /tmp/rafiq-bridge.pid); "
                                          "if test -r /proc/$p/stat; then "
                                          "awk '{print $3}' /proc/$p/stat; else echo dead; fi")
                        if response.strip() in ("dead", "Z"):
                            break
                print(f"pipeline ended; inspector exit={child.poll()}", flush=True)
                if time.monotonic() - started > 60:
                    backoff = 2
            except Exception as error:
                print(f"pipeline unavailable: {error}", flush=True)
            finally:
                stop_inspector(child)
                child = None
                try:
                    stop_little_bridge()
                except Exception as error:
                    print(f"little cleanup: {error}", flush=True)
            deadline = time.monotonic() + backoff
            while not STOPPING and time.monotonic() < deadline:
                time.sleep(0.5)
            backoff = min(backoff * 2, 60)
    finally:
        PID.unlink(missing_ok=True)
        lock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--daemon", action="store_true")
    mode.add_argument("--stop", action="store_true")
    parser.add_argument("--protection-v2", action="store_true")
    args = parser.parse_args()
    if args.stop:
        stop_service()
    elif not args.daemon or daemonize():
        supervise(args.protection_v2)


if __name__ == "__main__":
    main()
