#!/usr/bin/env python3
"""Stage and verify the two small-Linux executables over K230 shared memory."""

import hashlib
import mmap
import os
from pathlib import Path
import sys

ROOT = Path('/root/rafiq')
sys.path[:0] = [str(ROOT), '/root/rafiq-little-linux']
from k230_dual_linux_service import little, stop_little_bridge, clear_stopped_rings
from mvp_install_child import ensure_reader, wait_for_service_stop, TRANSFER_BASE, TRANSFER_SIZE


def deploy(source, destination, reader, window):
    payload = source.read_bytes()
    if not 64 <= len(payload) <= TRANSFER_SIZE or payload[:4] != b'\x7fELF':
        raise ValueError(f'invalid small-Linux executable: {source}')
    digest = hashlib.sha256(payload).hexdigest()
    window[:len(payload)] = payload
    staged = destination + '.next'
    output = little(f'{reader} /dev/mem {TRANSFER_BASE} {len(payload)} > {staged} && sha256sum {staged}', 60)
    if output.split()[0] != digest:
        raise RuntimeError(f'small-Linux SHA-256 mismatch: {destination}')
    little(f'chmod 755 {staged} && mv {staged} {destination}')
    current = little(f'sha256sum {destination}')
    if current.split()[0] != digest:
        raise RuntimeError(f'small-Linux installed binary mismatch: {destination}')
    print(destination, digest, flush=True)


def main():
    wait_for_service_stop()
    stop_little_bridge()
    clear_stopped_rings()
    reader = ensure_reader(little)
    fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
    try:
        with mmap.mmap(fd, TRANSFER_SIZE, offset=TRANSFER_BASE) as window:
            for name, staged in (
                ('k230-bridge', 'k230-bridge-little'),
                ('k230-mvp-force-stop-probe', 'k230-mvp-force-stop-probe-little'),
            ):
                deploy(ROOT / 'bin' / staged, '/root/rafiq/bin/' + name, reader, window)
    finally:
        os.close(fd)


if __name__ == '__main__':
    main()
