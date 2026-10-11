import gc
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
import numpy as np
from ai2d_runtime import HardwareAi2d

def handles():
    paths = []
    for fd in Path('/proc/self/fd').iterdir():
        try:
            paths.append(os.readlink(fd))
        except FileNotFoundError:
            pass
    counts = {kind: sum(kind in path for path in paths) for kind in ('/dev/mmz', 'ai2d')}
    counts['total'] = len(paths)
    counts['mmz_maps'] = sum('/dev/mmz' in line for line in Path('/proc/self/maps').read_text().splitlines())
    return counts

colors = np.array([71, 131, 207], dtype=np.uint8).reshape(1, 3, 1, 1)
before = handles()
print('before', before, flush=True)
with HardwareAi2d() as transform:
    output = np.zeros((1, 3, 224, 224), dtype=np.uint8)
    image = np.zeros((1, 3, 48, 64), dtype=np.uint8)
    image[:, :, 12:36, 16:48] = colors
    transform.run(image, output, (16, 12, 32, 24))
    assert np.abs(output.astype(np.int16) - colors).max() <= 1
    print('PASS RGB channels, crop, resize', handles(), flush=True)
    large_output = np.zeros((1, 3, 640, 640), dtype=np.uint8)
    for width, height in ((128, 64), (64, 128)):
        image = np.empty((1, 3, height, width), dtype=np.uint8)
        image[:] = colors
        transform.run(image, large_output, letterbox=True)
        left, top = (160, 0) if width < height else (0, 160)
        expected = np.full_like(large_output, 114)
        expected[:, :, top:640-top, left:640-left] = colors
        assert np.abs(large_output.astype(np.int16) - expected).max() <= 1
    print('PASS landscape and portrait letterbox', handles(), flush=True)
    image = np.empty((1, 3, 640, 286), dtype=np.uint8)
    image[:] = colors
    transform.run(image, output)
    steady = handles()
    for i in range(500):
        x = i % 40
        y = i % 120
        crop = (x, y, 286 - x - i % 50, 640 - y - i % 90)
        transform.run(image, output, crop)
        assert np.abs(output.astype(np.int16) - colors).max() <= 1
        assert handles() == steady, (i, steady, handles())
        if i % 100 == 0:
            print('PASS changing crop', i, handles(), flush=True)
gc.collect()
after = handles()
assert after['ai2d'] == before['ai2d'], (before, after)
assert after['/dev/mmz'] == before['/dev/mmz'] + 1, (before, after)
for i in range(20):
    with HardwareAi2d() as transform:
        transform.run(image, output)
        assert np.abs(output.astype(np.int16) - colors).max() <= 1
    assert handles() == after, (i, after, handles())
print('PASS 500 changed schedules and 20 reopenings; only the bounded runtime memory pool remains', after, flush=True)
