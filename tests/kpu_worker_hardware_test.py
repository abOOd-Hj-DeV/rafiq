"""Exercise the real AI2D/KPU worker with changing crops and monitor native handles."""

import argparse
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time

import numpy as np

parser = argparse.ArgumentParser()
parser.add_argument('--model', required=True)
parser.add_argument('--worker', default=str(Path(__file__).resolve().parent.parent / 'tools/kpu_worker.py'))
parser.add_argument('--requests', type=int, default=400)
parser.add_argument('--letterbox', action='store_true')
args = parser.parse_args()
if os.uname().machine != 'riscv64' or args.requests < 100:
    raise RuntimeError('This stress test requires K230 hardware and at least 100 requests')

parent, child = socket.socketpair()
parent.settimeout(30)
child_fd = child.fileno()

def setup():
    if child_fd != 3:
        os.dup2(child_fd, 3)
        os.close(child_fd)

command = [sys.executable, args.worker, '--model', args.model]
target, outputs = (640, 210000) if args.letterbox else (224, 5)
if args.letterbox:
    command += ['--letterbox', '--input-size', str(target), '--outputs', str(outputs)]
process = subprocess.Popen(command,
    pass_fds=tuple({3, child_fd}), preexec_fn=setup)
child.close()

def receive(size):
    data = bytearray()
    while len(data) < size:
        chunk = parent.recv(size - len(data))
        if not chunk:
            raise EOFError('KPU worker closed the channel')
        data.extend(chunk)
    return data

def resources():
    paths = []
    for fd in Path(f'/proc/{process.pid}/fd').iterdir():
        try:
            paths.append(os.readlink(fd))
        except FileNotFoundError:
            pass
    counts = {kind: sum(kind in path for path in paths) for kind in ('/dev/mmz', 'ai2d')}
    counts['total'] = len(paths)
    counts['mmz_maps'] = sum('/dev/mmz' in line for line in Path(f'/proc/{process.pid}/maps').read_text().splitlines())
    return counts

try:
    ready = struct.unpack('>4sIII', receive(16))
    assert ready == (b'RFR1', target, target, outputs), ready
    image = np.empty((1, 3, 640, 286), dtype=np.uint8)
    image[:] = np.array([71, 131, 207], dtype=np.uint8).reshape(1, 3, 1, 1)
    payload = image.tobytes()
    baseline = None
    started = time.monotonic()
    for i in range(args.requests):
        width, height = 286, 640
        if args.letterbox:
            width, height = ((286, 640), (640, 286), (320, 640), (640, 320))[i % 4]
            image = np.empty((1, 3, height, width), dtype=np.uint8)
            image[:] = np.array([71, 131, 207], dtype=np.uint8).reshape(1, 3, 1, 1)
            payload = image.tobytes()
            x, y, crop_width, crop_height = 0, 0, width, height
        else:
            x, y = i % 40, i % 120
            crop_width, crop_height = width - x - i % 50, height - y - i % 90
        parent.sendall(struct.pack('>4sIIIIIIqI', b'RFI1', width, height, x, y,
            crop_width, crop_height, i * 100_000, len(payload)) + payload)
        header = struct.unpack('>4sIqIIIII', receive(36))
        magic, status, pts, count, *_ = header
        assert magic == b'RFO1' and pts == i * 100_000, header
        if status:
            raise RuntimeError(receive(count).decode('utf-8', errors='replace'))
        assert count == outputs, count
        scores = np.frombuffer(receive(count * 4), dtype='>f4')
        assert np.isfinite(scores).all()
        current = resources()
        if i == 5:
            baseline = current
        if baseline is not None:
            assert current == baseline, (i, baseline, current)
        if i % 100 == 0:
            print('PASS real KPU changing crop', i, current, flush=True)
    print('PASS', args.requests, 'real AI2D/KPU inferences, stable handles/maps;',
        round(time.monotonic() - started, 2), 'seconds', baseline, flush=True)
finally:
    parent.close()
    try:
        returncode = process.wait(timeout=15)
        assert returncode == 0, returncode
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=5)
        raise
