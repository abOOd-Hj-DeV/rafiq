#!/usr/bin/env python3
"""Stream the verified Mentor APK through little Linux into an Android session."""

import argparse
import base64
import fcntl
import gzip
import hashlib
import mmap
import os
from pathlib import Path
import re
import shlex
import signal
import struct
import subprocess
import sys
import time

ROOT = Path("/root/rafiq")
SERVICE_PID = Path("/var/run/rafiq-dual-linux.pid")
TRANSFER_BASE = 0x0C000000
TRANSFER_SIZE = 16 * 1024 * 1024


def wait_for_service_stop() -> None:
    if SERVICE_PID.exists():
        pid = int(SERVICE_PID.read_text())
        command = Path(f"/proc/{pid}/cmdline")
        if command.exists() and b"k230_dual_linux_service.py" in command.read_bytes():
            os.kill(pid, signal.SIGTERM)
    deadline = time.monotonic() + 45
    while SERVICE_PID.exists() and time.monotonic() < deadline:
        time.sleep(0.1)
    if SERVICE_PID.exists():
        raise RuntimeError("MVP service did not stop")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def ensure_reader(little) -> str:
    binary = ROOT / "bin/k230-shmem-read-little"
    expected = sha256(binary)
    reader = "/root/rafiq/bin/k230-shmem-read"
    existing = little(f"test ! -x {reader} || sha256sum {reader}")
    if existing.startswith(expected + " "):
        return reader
    encoded = base64.b64encode(gzip.compress(binary.read_bytes(), mtime=0)).decode("ascii")
    temporary = reader + ".b64"
    little(f"mkdir -p /root/rafiq/bin; : > {temporary}")
    for offset in range(0, len(encoded), 2800):
        little(f"printf '%s' '{encoded[offset:offset + 2800]}' >> {temporary}")
    output = little(
        f"base64 -d {temporary} | gzip -d > {reader}.next; sha256sum {reader}.next"
    )
    if not output.startswith(expected + " "):
        raise RuntimeError("Little memory reader checksum mismatch")
    little(f"chmod 755 {reader}.next && mv {reader}.next {reader}; rm -f {temporary}")
    return reader


def transfer(args, fd: int) -> bool:
    sys.path.insert(0, "/root/rafiq-little-linux")
    from little_linux import MAILBOX, execute, preflight

    preflight()
    installed = False
    session = None
    directory = f"/root/rafiq/.install-{os.getpid()}"
    size = args.apk.stat().st_size
    with open("/var/run/rafiq-little-mailbox.lock", "a") as lock, mmap.mmap(
        fd, 0x100000, offset=MAILBOX
    ) as mailbox:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if struct.unpack_from("<Q", mailbox, 0x30008)[0] != 0x52414649514C4E58:
            raise RuntimeError("Little Linux is not ready")

        def little(command: str, timeout: int = 45) -> str:
            output, status = execute(mailbox, command, timeout)
            if status:
                raise RuntimeError(f"little command failed ({status}): {output}")
            return output

        little(
            "for p in /proc/[0-9]*/exe; do "
            "if test \"$(readlink $p 2>/dev/null)\" = /root/rafiq/bin/k230-bridge; "
            "then exit 9; fi; done"
        )
        devices = [
            line.split()[0] for line in little("adb devices").splitlines()
            if len(line.split()) == 2 and line.split()[1] == "device"
        ]
        if len(devices) != 1:
            raise RuntimeError("Expected exactly one authorized child phone")
        reader = ensure_reader(little)
        adb = "adb -s " + shlex.quote(devices[0]) + " shell -T pm "
        try:
            output = little(adb + f"install-create -r --user current -S {size}")
            match = re.search(r"Success: created install session \[(\d+)\]", output)
            if not match:
                raise RuntimeError("Android did not create an install session: " + output)
            session = int(match[1])
            little(
                f"mkdir -p {directory} && busybox mkfifo {directory}/data {directory}/hash "
                f"&& touch {directory}/writing"
            )
            little(
                "trap '' HUP; "
                f"(sha256sum < {directory}/hash > {directory}/sha256) 2>{directory}/hash-error & "
                f"(busybox tee {directory}/hash < {directory}/data | "
                f"{adb}install-write -S {size} {session} base - > {directory}/write-log; "
                f"echo $? > {directory}/write-exit) </dev/null >{directory}/worker-log 2>&1 & "
                f"(exec 3>{directory}/data; while test -e {directory}/writing; do sleep 1; done) "
                "</dev/null >/dev/null 2>&1 &"
            )
            transferred = 0
            with args.apk.open("rb") as source, mmap.mmap(
                fd, TRANSFER_SIZE, offset=TRANSFER_BASE
            ) as window:
                for chunk in iter(lambda: source.read(TRANSFER_SIZE), b""):
                    window[:len(chunk)] = chunk
                    command = f"{reader} /dev/mem {TRANSFER_BASE} {len(chunk)} > {directory}/data"
                    little(command, 90)
                    transferred += len(chunk)
                    print(f"Transferred {transferred}/{size}", flush=True)
            little(
                f"rm -f {directory}/writing; i=0; "
                f"while ! test -s {directory}/sha256 || ! test -s {directory}/write-exit; do "
                "i=$((i+1)); if test $i -ge 90; then exit 8; fi; sleep 1; done",
                100,
            )
            digest = little(f"cat {directory}/sha256").split()[0]
            output = little(f"cat {directory}/write-log; cat {directory}/write-exit")
            if digest != args.sha256.lower() or "Success" not in output or not output.rstrip().endswith("0"):
                raise RuntimeError("APK stream/write verification failed: " + output)
            output = little(adb + f"install-commit {session}", 180)
            if "Success" not in output:
                raise RuntimeError("Android rejected the APK: " + output)
            installed = True
            print("PASS: streamed, SHA256 verified and installed Mentor MVP", flush=True)
        finally:
            try:
                little(f"rm -f {directory}/writing")
                if session is not None and not installed:
                    little(adb + f"install-abandon {session}")
            except Exception as error:
                print(f"Install cleanup: {error}", file=sys.stderr)
    return installed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apk", required=True, type=Path)
    parser.add_argument("--sha256", required=True)
    args = parser.parse_args()
    if not args.apk.is_file() or sha256(args.apk) != args.sha256.lower():
        raise RuntimeError("APK checksum mismatch")
    wait_for_service_stop()
    with open(ROOT / "service.lock", "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        for proc in Path("/proc").glob("[0-9]*"):
            try:
                command = (proc / "cmdline").read_bytes().split(b"\0")[0]
            except (FileNotFoundError, PermissionError, ProcessLookupError):
                continue
            if command == str(ROOT / "bin/k230-inspector").encode():
                raise RuntimeError("Another inspector owns the transfer region")
        fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
        try:
            installed = transfer(args, fd)
        finally:
            os.close(fd)
    if installed:
        subprocess.run(["python3", str(ROOT / "k230_dual_linux_service.py"), "--daemon"], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
