"""Owning hardware AI2D adapter. Missing hardware support fails closed."""

import ctypes
from pathlib import Path


class HardwareAi2d:
    def __init__(self, library=None):
        self.library = ctypes.CDLL(str(library or Path(__file__).with_name("libk230_ai2d.so")))
        self.library.k230_ai2d_abi.restype = ctypes.c_uint32
        if self.library.k230_ai2d_abi() != 0x52414931:
            raise RuntimeError("unsupported AI2D adapter ABI")
        self.library.k230_ai2d_create.restype = ctypes.c_void_p
        self.library.k230_ai2d_destroy.argtypes = [ctypes.c_void_p]
        self.library.k230_ai2d_destroy.restype = None
        self.library.k230_ai2d_error.argtypes = [ctypes.c_void_p]
        self.library.k230_ai2d_error.restype = ctypes.c_char_p
        self.library.k230_ai2d_run.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p, ctypes.c_size_t]
        self.library.k230_ai2d_run.restype = ctypes.c_int
        self.handle = self.library.k230_ai2d_create()
        if not self.handle:
            raise RuntimeError("AI2D context allocation failed")

    def close(self):
        if self.handle:
            self.library.k230_ai2d_destroy(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def run(self, source, output, crop=None, letterbox=False):
        if not self.handle:
            raise RuntimeError("AI2D context is closed")
        if (source.dtype.name != "uint8" or output.dtype.name != "uint8" or
                source.ndim != 4 or output.ndim != 4 or source.shape[:2] != (1, 3) or
                output.shape[:2] != (1, 3) or output.shape[2] != output.shape[3] or
                not source.flags.c_contiguous or not output.flags.c_contiguous or
                not output.flags.writeable):
            raise ValueError("AI2D requires contiguous NCHW uint8 arrays")
        height, width = source.shape[2:]
        x, y, crop_width, crop_height = crop or (0, 0, width, height)
        parameters = (width, height, x, y, crop_width, crop_height, output.shape[2], int(letterbox))
        if any(type(value) is not int or not 0 <= value <= 0xFFFFFFFF for value in parameters):
            raise ValueError("invalid AI2D dimensions")
        key = (ctypes.c_uint32 * 8)(*parameters)
        if self.library.k230_ai2d_run(self.handle, source.ctypes.data, source.nbytes,
                key, output.ctypes.data, output.nbytes) != 0:
            raise RuntimeError(self.library.k230_ai2d_error(self.handle).decode("utf-8", errors="replace"))
