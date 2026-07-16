"""Named-pipe client for the injected Grandiarchipelago.dll."""

from __future__ import annotations

import ctypes
import logging
from ctypes import wintypes
from typing import Optional

logger = logging.getLogger("GrandiaClient")

PIPE_NAME = r"\\.\pipe\Grandiarchipelago"

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.CreateFileW.argtypes = [
    wintypes.LPCWSTR,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.LPVOID,
    wintypes.DWORD,
    wintypes.DWORD,
    wintypes.HANDLE,
]
_k32.CreateFileW.restype = wintypes.HANDLE
_k32.WriteFile.argtypes = [
    wintypes.HANDLE,
    wintypes.LPCVOID,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    wintypes.LPVOID,
]
_k32.WriteFile.restype = wintypes.BOOL
_k32.ReadFile.argtypes = [
    wintypes.HANDLE,
    wintypes.LPVOID,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    wintypes.LPVOID,
]
_k32.ReadFile.restype = wintypes.BOOL
_k32.PeekNamedPipe.argtypes = [
    wintypes.HANDLE,
    wintypes.LPVOID,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
    ctypes.POINTER(wintypes.DWORD),
    ctypes.POINTER(wintypes.DWORD),
]
_k32.PeekNamedPipe.restype = wintypes.BOOL
_k32.CloseHandle.argtypes = [wintypes.HANDLE]
_k32.CloseHandle.restype = wintypes.BOOL
_k32.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
_k32.WaitNamedPipeW.restype = wintypes.BOOL

_INVALID = ctypes.c_void_p(-1).value


class GamePipe:
    def __init__(self) -> None:
        self._handle: Optional[int] = None
        self._buf = bytearray()

    @property
    def connected(self) -> bool:
        return self._handle is not None

    def connect(self, timeout_ms: int = 60000) -> bool:
        if not _k32.WaitNamedPipeW(PIPE_NAME, timeout_ms):
            logger.error("WaitNamedPipe timed out for %s", PIPE_NAME)
            return False
        handle = _k32.CreateFileW(
            PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0,
            None,
            OPEN_EXISTING,
            0,
            None,
        )
        if not handle or handle == _INVALID:
            logger.error("CreateFile on game pipe failed (%s)", ctypes.get_last_error())
            return False
        self._handle = int(handle)
        logger.info("Connected to game pipe")
        return True

    def close(self) -> None:
        if self._handle:
            _k32.CloseHandle(self._handle)
            self._handle = None
        self._buf.clear()

    def send_line(self, line: str) -> bool:
        if not self._handle:
            return False
        payload = (line + "\n").encode("utf-8")
        written = wintypes.DWORD()
        ok = _k32.WriteFile(self._handle, payload, len(payload), ctypes.byref(written), None)
        return bool(ok)

    def poll_line(self) -> Optional[str]:
        """Non-blocking: return one complete line if available."""
        if not self._handle:
            return None
        avail = wintypes.DWORD()
        if not _k32.PeekNamedPipe(self._handle, None, 0, None, ctypes.byref(avail), None):
            err = ctypes.get_last_error()
            logger.warning("PeekNamedPipe failed (%s) — pipe lost", err)
            self.close()
            return None
        if avail.value == 0:
            if b"\n" in self._buf:
                return self._pop_line()
            return None

        chunk = (ctypes.c_ubyte * avail.value)()
        read = wintypes.DWORD()
        if not _k32.ReadFile(self._handle, chunk, avail.value, ctypes.byref(read), None):
            logger.warning("ReadFile failed — pipe lost")
            self.close()
            return None
        self._buf.extend(bytes(chunk)[: read.value])
        return self._pop_line()

    def _pop_line(self) -> Optional[str]:
        nl = self._buf.find(b"\n")
        if nl < 0:
            return None
        raw = bytes(self._buf[:nl])
        del self._buf[: nl + 1]
        return raw.decode("utf-8", errors="replace").rstrip("\r")
