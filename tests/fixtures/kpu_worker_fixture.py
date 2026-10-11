"""Socket protocol fixture only; never used as a production model backend."""
import socket
import struct
import sys
import time

mode = sys.argv[sys.argv.index("--model") + 1]
input_size = int(sys.argv[sys.argv.index("--input-size") + 1])
outputs = int(sys.argv[sys.argv.index("--outputs") + 1])
audio = "--audio" in sys.argv
with socket.socket(fileno=3) as connection:
    connection.setblocking(True)
    if mode == "startup-stall":
        time.sleep(10)
        sys.exit(0)
    connection.sendall(struct.pack(
        ">4sIII", b"RFR1", 640 if mode == "wrong-shape" else input_size, 64 if audio else input_size, outputs))
    while True:
        request = bytearray()
        while len(request) < 40:
            part = connection.recv(40 - len(request))
            if not part:
                sys.exit(0)
            request.extend(part)
        magic, w, h, x, y, cw, ch, pts, size = struct.unpack(">4sIIIIIIqI", request)
        data = bytearray()
        while len(data) < size:
            part = connection.recv(size - len(data))
            if not part:
                sys.exit(1)
            data.extend(part)
        if audio:
            if (magic != b"RFA1" or w != 16000 or h != 2 or x != 16000 or y or cw or ch
                    or size != 64000 or data != struct.pack(">h", -1234) * 32000):
                sys.exit(2)
        elif size != w * h * 3 or (x, y, cw, ch) != (0, 0, w, h) or data != bytes([73]) * size:
            sys.exit(2)
        if mode == "infer-stall":
            time.sleep(10)
            sys.exit(0)
        if mode == "eof":
            sys.exit(0)
        if mode == "error":
            error = b"hardware inference rejected"
            connection.sendall(struct.pack(">4sIqIIIII", b"RFO1", 1, pts, len(error), 0, 0, 0, 0) + error)
            continue
        values = [0.1, 0.1, 0.2, 0.3, 0.3] if outputs == 5 else [0.0] * outputs
        if audio:
            values = [.2] * outputs
            values[12] = .8
        if mode == "nan":
            values[0] = float("nan")
        if mode == "bad-range":
            values[0] = 1.01
        if mode == "bad-sum":
            values = [0.0] * outputs
        response = struct.pack(
            ">4sIqIIIII", b"RFO1", 0, pts + (mode == "wrong-pts"), outputs, 1000, 2000, 3000, 0)
        for offset in range(0, len(response), 3):
            connection.sendall(response[offset:offset + 3])
        payload = struct.pack(f">{outputs}f", *values)
        chunk_size = 4093 if outputs > 4096 else 3
        for offset in range(0, len(payload), chunk_size):
            connection.sendall(payload[offset:offset + chunk_size])
