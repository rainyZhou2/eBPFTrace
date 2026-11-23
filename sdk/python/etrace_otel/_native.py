import ctypes
import ctypes.util
import os

_LIB_ENV = "ETRACE_CTX_LIBRARY"


class _Native:
    def __init__(self):
        self._lib = None
        path = os.environ.get(_LIB_ENV) or ctypes.util.find_library("etrace_ctx")
        if not path:
            return
        try:
            lib = ctypes.CDLL(path)
        except OSError:
            return
        lib.etrace_set_context.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        lib.etrace_set_context.restype = None
        lib.etrace_clear_context.argtypes = []
        lib.etrace_clear_context.restype = None
        self._lib = lib

    @property
    def available(self):
        return self._lib is not None

    def set(self, trace_id: int, span_id: int) -> None:
        if self._lib is None:
            return
        self._lib.etrace_set_context(trace_id.to_bytes(16, "big"), span_id.to_bytes(8, "big"))

    def clear(self) -> None:
        if self._lib is not None:
            self._lib.etrace_clear_context()


native = _Native()
