#!/usr/bin/env python3
"""Relay aggregate child metrics from K230 ADB to the local guardian emulator."""

import argparse
import re
import subprocess
import time


def adb(adb_path, serial, *command):
    result = subprocess.run([adb_path, "-s", serial, "shell", *command],
                            capture_output=True, text=True, timeout=45)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or "ADB command failed")
    return result.stdout


def board_export():
    return (
        "import sys;sys.path[:0]=['/root/rafiq','/root/rafiq-little-linux'];"
        "from k230_dual_linux_service import little;"
        "devices=little('adb devices');"
        "ids=[x.split()[0] for x in devices.splitlines() if len(x.split())==2 and x.split()[1]=='device'];"
        "assert len(ids)==1,'Expected one authorized child';"
        "import shlex;"
        "print(little(shlex.join(['adb','-s',ids[0],'shell','am','broadcast','--user','current',"
        "'-a','dev.k230.mentor.MVP_EXPORT','-n','dev.k230.mentor_app/.MvpReportReceiver']),20))"
    )


def extract_report(output):
    match = re.search(r'REPORT:([A-Za-z0-9+/=]{16,8192})', output)
    if not match:
        raise RuntimeError("Child did not return a report")
    return match[1]


def relay_once(adb_path, board, guardian):
    # ADB shell joins arguments into a remote command; quote the Python source once.
    import shlex
    report = extract_report(adb(adb_path, board, "python3", "-c", shlex.quote(board_export())))
    output = adb(adb_path, guardian, "am", "broadcast", "--user", "current",
                 "-a", "dev.k230.mentor.MVP_IMPORT", "-n",
                 "dev.k230.mentor_app/.MvpReportReceiver", "--es", "snapshot", report)
    if not re.search(r"result=0\b", output):
        raise RuntimeError("Guardian rejected report; choose the guardian role first")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--board", required=True)
    parser.add_argument("--guardian", default="emulator-5554")
    parser.add_argument("--once", action="store_true")
    args = parser.parse_args()
    while True:
        try:
            relay_once(args.adb, args.board, args.guardian)
            print("Guardian aggregate report synchronized", flush=True)
        except (RuntimeError, subprocess.SubprocessError) as error:
            print(f"Report not synchronized: {error}", flush=True)
            if args.once:
                return 1
        if args.once:
            return 0
        time.sleep(10)


if __name__ == "__main__":
    raise SystemExit(main())
