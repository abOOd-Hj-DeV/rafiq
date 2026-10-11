#!/usr/bin/env python3
"""Persistent official nncase 2.11 AI2D/KPU worker; no software image fallback."""

import argparse
import os
from pathlib import Path
import socket
import struct
import sys
import time

from ai2d_runtime import HardwareAi2d


def validate_model(data):
    if len(data) < 32 or len(data) > 512 * 1024 * 1024:
        raise ValueError("invalid KModel size")
    magic, version, _, alignment, modules, entry, _, _ = struct.unpack_from("<8I", data)
    if magic != 0x4B4D444C or version != 7 or alignment not in (8, 16, 32, 64):
        raise ValueError("expected nncase model version 7")
    if not 1 <= modules <= 32 or entry >= modules:
        raise ValueError("invalid KModel module table")
    offset, kinds = 32, []
    for _ in range(modules):
        offset = (offset + alignment - 1) // alignment * alignment
        if offset + 40 > len(data):
            raise ValueError("truncated KModel module header")
        kind, _, _, functions, _, size = struct.unpack_from("<16s4IQ", data, offset)
        if size < 40 or offset + size > len(data) or not functions:
            raise ValueError("invalid KModel module bounds")
        kinds.append(kind.split(b"\0", 1)[0])
        offset += size
    if b"k230" not in kinds:
        raise ValueError("KModel has no K230 hardware module")


def receive(connection, size):
    data = bytearray()
    while len(data) < size:
        chunk = connection.recv(size - len(data))
        if not chunk:
            if not data:
                return None
            raise EOFError("truncated KPU request")
        data.extend(chunk)
    return data


def verify_ai2d(np):
    image = np.zeros((1, 3, 48, 64), dtype=np.uint8)
    colors = np.array([71, 131, 207], dtype=np.uint8).reshape(1, 3, 1, 1)
    image[:, :, 12:36, 16:48] = colors
    pixels = np.zeros((1, 3, 224, 224), dtype=np.uint8)
    with HardwareAi2d() as transform:
        transform.run(image, pixels, (16, 12, 32, 24))
    if pixels.shape != (1, 3, 224, 224) or np.abs(pixels.astype(np.int16) - colors).max() > 1:
        raise RuntimeError("AI2D RGB channel/crop/resize pixel verification failed")


def verify_letterbox(np):
    colors = np.array([71, 131, 207], dtype=np.uint8).reshape(1, 3, 1, 1)
    for width, height in ((128, 64), (64, 128)):
        image = np.empty((1, 3, height, width), dtype=np.uint8)
        image[:] = colors
        pixels = np.zeros((1, 3, 640, 640), dtype=np.uint8)
        left, top = (160, 0) if width < height else (0, 160)
        with HardwareAi2d() as transform:
            transform.run(image, pixels, letterbox=True)
        expected = np.full((1, 3, 640, 640), 114, dtype=np.uint8)
        expected[:, :, top:640-top, left:640-left] = colors
        if np.abs(pixels.astype(np.int16) - expected).max() > 1:
            raise RuntimeError("AI2D portrait/landscape letterbox pixel verification failed")


def run_audio(interpreter, runtime, np):
    from audio_features import yamnet_pcm_features
    with socket.socket(fileno=3) as connection:
        connection.setblocking(True)
        connection.sendall(struct.pack(">4sIII", b"RFR1", 96, 64, 521))
        while True:
            header = receive(connection, 40)
            if header is None:
                return
            magic, rate, channels, frames, x, y, z, pts, size = struct.unpack(">4sIIIIIIqI", header)
            try:
                if (magic != b"RFA1" or rate not in (16000, 48000) or channels not in (1, 2)
                        or not rate * 975 // 1000 <= frames <= rate * 10 or x or y or z
                        or size != frames * channels * 2
                        or pts < frames * 1_000_000 // rate):
                    raise ValueError("invalid YAMNet PCM request")
                data = receive(connection, size)
                if data is None:
                    raise EOFError("missing PCM data")
                started = time.monotonic_ns()
                pcm = np.frombuffer(data, dtype=">i2").astype(np.float32).reshape(-1, channels)
                mono = pcm.mean(axis=1) / np.float32(32768)
                features = yamnet_pcm_features(mono, rate)
                tensor = runtime.RuntimeTensor.from_numpy(features)
                prepared = time.monotonic_ns()
                interpreter.set_input_tensor(0, tensor)
                interpreter.run()
                inferred = time.monotonic_ns()
                logits = interpreter.get_output_tensor(0).to_numpy()
                if logits.dtype != np.float32 or logits.shape != (1, 521) or not np.isfinite(logits).all():
                    raise ValueError("invalid YAMNet logits")
                values = np.exp(-np.logaddexp(np.float32(0), -logits))
                connection.sendall(struct.pack(">4sIqIIIII", b"RFO1", 0, pts, values.size,
                                               (prepared - started) // 1000, 0,
                                               (inferred - prepared) // 1000, 0))
                connection.sendall(values.astype(">f4").tobytes())
            except Exception as error:
                message = str(error).encode("utf-8")[:4096]
                connection.sendall(struct.pack(">4sIqIIIII", b"RFO1", 1, pts, len(message), 0, 0, 0, 0) + message)
                raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--input-size", type=int, default=224)
    parser.add_argument("--outputs", type=int, default=5)
    parser.add_argument("--letterbox", action="store_true")
    parser.add_argument("--audio", action="store_true")
    args = parser.parse_args()
    expected_output = [1, 521] if args.audio else [1, 25, 8400] if args.letterbox else [1, 5]
    contract = (96, 521) if args.audio else (640, 210000) if args.letterbox else (224, 5)
    if (args.input_size, args.outputs) != contract or (args.audio and args.letterbox):
        raise ValueError("unsupported KPU model contract")
    if os.uname().machine != "riscv64":
        raise RuntimeError("K230 hardware runtime requires RISC-V Linux")
    path = Path(args.model)
    if not 32 <= path.stat().st_size <= 512 * 1024 * 1024:
        raise ValueError("invalid KModel file size")
    validate_model(path.read_bytes())

    import numpy as np
    import nncaseruntime as runtime

    if not args.audio:
        verify_ai2d(np)
    if args.letterbox:
        verify_letterbox(np)
    interpreter = runtime.Interpreter()
    interpreter.load_model(str(path))
    if interpreter.inputs_size != 1 or interpreter.outputs_size != 1:
        raise ValueError("KPU model requires one input and one output")
    shape = list(interpreter.get_input_shape(0))
    output_shape = list(interpreter.get_output_shape(0))
    expected_input = [1, 1, 96, 64] if args.audio else [1, 3, args.input_size, args.input_size]
    if shape != expected_input or output_shape != expected_output:
        raise ValueError("unexpected KPU model tensor shape")
    if interpreter.get_input_desc(0).dtype != np.dtype("float32" if args.audio else "uint8"):
        raise ValueError("unexpected KModel input dtype")
    if interpreter.get_output_desc(0).dtype != np.dtype("float32"):
        raise ValueError("KModel output must be float32")
    if args.audio:
        return run_audio(interpreter, runtime, np)
    output_array = np.zeros(shape, dtype=np.uint8)
    with HardwareAi2d() as ai2d, socket.socket(fileno=3) as connection:
        connection.setblocking(True)
        connection.sendall(struct.pack(">4sIII", b"RFR1", args.input_size, args.input_size, args.outputs))
        while True:
            header = receive(connection, 40)
            if header is None:
                return
            magic, width, height, x, y, crop_width, crop_height, pts, size = struct.unpack(">4sIIIIIIqI", header)
            try:
                if (magic != b"RFI1" or not 0 < width <= 4096 or not 0 < height <= 4096
                        or size != width * height * 3 or size > 12 * 1024 * 1024 or pts < 0
                        or not 0 < crop_width <= width or not 0 < crop_height <= height
                        or x + crop_width > width or y + crop_height > height):
                    raise ValueError("invalid RGB frame or crop")
                data = receive(connection, size)
                if data is None:
                    raise EOFError("missing RGB data")
                started = time.monotonic_ns()
                source_array = np.frombuffer(data, dtype=np.uint8).reshape(1, 3, height, width)
                prepared = time.monotonic_ns()
                ai2d.run(source_array, output_array, (x, y, crop_width, crop_height), args.letterbox)
                transformed = time.monotonic_ns()
                output = runtime.RuntimeTensor.from_numpy(output_array)
                interpreter.set_input_tensor(0, output)
                interpreter.run()
                inferred = time.monotonic_ns()
                values = interpreter.get_output_tensor(0).to_numpy()
                if (values.dtype != np.float32 or values.size != args.outputs or not np.isfinite(values).all()):
                    raise ValueError("invalid KPU output")
                connection.sendall(struct.pack(">4sIqIIIII", b"RFO1", 0, pts, values.size,
                                               (prepared - started) // 1000,
                                               (transformed - prepared) // 1000,
                                               (inferred - transformed) // 1000, 0))
                connection.sendall(values.astype(">f4").tobytes())
            except Exception as error:
                message = str(error).encode("utf-8")[:4096]
                connection.sendall(struct.pack(">4sIqIIIII", b"RFO1", 1, pts, len(message), 0, 0, 0, 0) + message)
                raise


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"KPU worker failed: {error}; no software fallback", file=sys.stderr, flush=True)
        sys.exit(1)
