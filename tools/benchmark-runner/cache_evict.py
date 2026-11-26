"""Best-effort eviction of ONE file from the Windows file cache, without administrator rights.

Method (`unbuffered-open`): CreateFileW(..., FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH) on the file and
read one sector. When a file that has cached pages is opened for unbuffered access the cache manager flushes and
(if no section is mapped) purges the file's cached data. Whether that really evicted the pages is NOT assumed:
`verify()` times a sequential read of a prefix before/after and the caller reports the measured numbers.

This only READS the file. It never writes, truncates or deletes anything.
"""
import ctypes
import os
import sys
import time

GENERIC_READ = 0x80000000
FILE_SHARE_ALL = 0x7
OPEN_EXISTING = 3
FILE_FLAG_NO_BUFFERING = 0x20000000
FILE_FLAG_WRITE_THROUGH = 0x80000000
INVALID_HANDLE = ctypes.c_void_p(-1).value


def evict(path: str) -> bool:
    """Returns True when the unbuffered open succeeded (it does NOT prove eviction; see verify)."""
    if os.name != "nt":
        return False
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateFileW.restype = ctypes.c_void_p
    k32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
                                ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    h = k32.CreateFileW(path, GENERIC_READ, FILE_SHARE_ALL, None, OPEN_EXISTING,
                        FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, None)
    if h in (None, INVALID_HANDLE):
        return False
    try:
        buf = ctypes.create_string_buffer(65536)  # sector-aligned size; the buffer address is page-aligned enough
        n = ctypes.c_uint32(0)
        k32.ReadFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32),
                                 ctypes.c_void_p]
        k32.ReadFile(h, buf, 65536, ctypes.byref(n), None)
    finally:
        k32.CloseHandle.argtypes = [ctypes.c_void_p]
        k32.CloseHandle(h)
    return True


def read_prefix_mib_s(path: str, mib: int = 256) -> float:
    """Sequential buffered read of the first `mib` MiB; returns MiB/s (the file stays cached afterwards)."""
    want = mib << 20
    got = 0
    t0 = time.perf_counter()
    with open(path, "rb", buffering=0) as f:
        while got < want:
            b = f.read(min(16 << 20, want - got))
            if not b:
                break
            got += len(b)
    dt = time.perf_counter() - t0
    return (got / (1 << 20)) / dt if dt > 0 else 0.0


def verify(path: str, mib: int = 256) -> dict:
    """warm -> evict -> read: the evict is considered effective when the read after it is at least 2x slower than
    the warm read."""
    read_prefix_mib_s(path, mib)  # make sure it is cached
    warm = read_prefix_mib_s(path, mib)
    opened = evict(path)
    after = read_prefix_mib_s(path, mib)
    return {"method": "unbuffered-open", "open_ok": opened, "warm_mib_s": round(warm, 1),
            "after_evict_mib_s": round(after, 1), "effective": bool(opened and after * 2 < warm)}


if __name__ == "__main__":
    import json
    print(json.dumps(verify(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 256), indent=2))
