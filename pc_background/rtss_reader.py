"""Read-only RTSSSharedMemoryV2 reader; returns no FPS when RTSS is unavailable.

Uses documented RTSS v2 application entry fields, does not patch processes,
install hooks, or request elevated privileges. Windows only.
"""
from __future__ import annotations
import ctypes
import os
import struct
import time
from ctypes import wintypes

MAGIC = b"PULSEPANEL_DISCOVER_V1"


def parse_rtss_v2(blob: bytes, previous: dict[int, tuple[int, float]], now: float,
                  foreground_pid: int | None = None) -> dict | None:
    """Decode RTSS v2 app table. Reject out-of-bounds and stale frame counters."""
    if len(blob) < 36:
        return None
    signature, version, entry_size, offset, count, _, _, _, _ = struct.unpack_from('<4sIIIIIIII', blob)
    if signature not in (b'RTSS', b'SSTR') or not 0x00020000 <= version < 0x00040000:
        return None
    if entry_size < 284 or entry_size > 8192 or count > 1024 or offset < 36:
        return None
    if offset + entry_size * count > len(blob):
        return None
    candidates = []
    for idx in range(count):
        pos = offset + idx * entry_size
        pid, name, flags, t0, t1, frames, frame_time = struct.unpack_from('<I260sIIIII', blob, pos)
        if not pid or not t0 or not frames or not t1:
            continue
        delta = (t1 - t0) & 0xFFFFFFFF
        if not 0 < delta < 30000:
            continue
        fps = round(frames * 1000.0 / delta)
        if fps < 1 or fps > 1000:
            continue
        prev = previous.get(pid)
        if prev is None or prev[0] != t1:
            previous[pid] = (t1, now)
        if now - previous[pid][1] > 3.5:
            continue
        game = name.split(b'\0',1)[0].decode('ascii', errors='replace').replace('?', '_')[:32]
        candidates.append({'fps': fps, 'frametime_ms': frame_time / 1000 if 0 < frame_time < 300000 else None,
                           'game': game, 'pid': pid, 'foreground': pid == foreground_pid})
    if not candidates:
        return None
    candidates.sort(key=lambda x: (x['foreground'], x['fps']), reverse=True)
    return candidates[0]


class RTSSReader:
    def __init__(self):
        self.previous: dict[int, tuple[int, float]] = {}
        self.available = os.name == 'nt'
        if self.available:
            self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            self.user = ctypes.WinDLL('user32', use_last_error=True)
            self.kernel.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
            self.kernel.OpenFileMappingW.restype = wintypes.HANDLE
            self.kernel.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                                  wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
            self.kernel.MapViewOfFile.restype = ctypes.c_void_p
            self.kernel.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
            self.kernel.UnmapViewOfFile.restype = wintypes.BOOL
            self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            self.kernel.CloseHandle.restype = wintypes.BOOL
            self.user.GetForegroundWindow.restype = wintypes.HWND
            self.user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
            self.user.GetWindowThreadProcessId.restype = wintypes.DWORD

    def read(self) -> dict | None:
        if not self.available:
            return None
        h = self.kernel.OpenFileMappingW(0x0004, False, 'RTSSSharedMemoryV2')
        if not h:
            return None
        try:
            # Map the known fixed 36-byte header first, then cap the variable region.
            view = self.kernel.MapViewOfFile(h, 0x0004, 0, 0, 36)
            if not view:
                return None
            try:
                header = ctypes.string_at(view, 36)
            finally:
                self.kernel.UnmapViewOfFile(view)
            if header[:4] not in (b'RTSS', b'SSTR'):
                return None
            _, v, sz, off, count, *_ = struct.unpack('<4sIIIIIIII', header)
            if not 0x00020000 <= v < 0x00040000 or sz < 284 or sz > 8192 or count > 1024 or off < 36:
                return None
            length = off + sz * count
            if not 36 <= length <= 8 * 1024 * 1024:
                return None
            view = self.kernel.MapViewOfFile(h, 0x0004, 0, 0, length)
            if not view:
                return None
            try:
                blob = ctypes.string_at(view, length)
            finally:
                self.kernel.UnmapViewOfFile(view)
            pid = wintypes.DWORD()
            self.user.GetWindowThreadProcessId(self.user.GetForegroundWindow(), ctypes.byref(pid))
            return parse_rtss_v2(blob, self.previous, time.monotonic(), pid.value)
        finally:
            self.kernel.CloseHandle(h)
