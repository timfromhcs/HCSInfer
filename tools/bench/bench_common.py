"""Shared helpers for the wave-D benchmark driver: process runs with peak-memory sampling (Windows)."""
from __future__ import annotations
import ctypes, hashlib, json, os, subprocess, sys, time, zlib, struct
from ctypes import wintypes

class _PMC(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
                ("PrivateUsage", ctypes.c_size_t)]

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.K32GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(_PMC), wintypes.DWORD]

def _mem(handle):
    pmc = _PMC(); pmc.cb = ctypes.sizeof(pmc)
    if _k32.K32GetProcessMemoryInfo(wintypes.HANDLE(int(handle)), ctypes.byref(pmc), pmc.cb):
        return pmc.PeakWorkingSetSize, pmc.PeakPagefileUsage
    return None

def run(cmd, timeout=None, cwd=None, env=None):
    """Run cmd; return dict(rc, wall_s, stdout, stderr, peak_ws_bytes, peak_commit_bytes, timed_out)."""
    t0 = time.perf_counter()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=cwd, env=env)
    out, err = [], []
    import threading
    def pump(s, sink):
        for ch in iter(lambda: s.read(65536), b""): sink.append(ch)
    th = [threading.Thread(target=pump, args=(p.stdout, out), daemon=True),
          threading.Thread(target=pump, args=(p.stderr, err), daemon=True)]
    for t in th: t.start()
    last, timed_out = None, False
    while p.poll() is None:
        m = _mem(p._handle)
        if m: last = m
        if timeout and time.perf_counter() - t0 > timeout:
            p.kill(); timed_out = True; break
        time.sleep(0.2)
    p.wait()
    for t in th: t.join(5)
    m = _mem(p._handle)
    if m: last = m
    return dict(rc=p.returncode, wall_s=time.perf_counter() - t0, timed_out=timed_out,
                stdout=b"".join(out).decode("utf-8", "replace"), stderr=b"".join(err).decode("utf-8", "replace"),
                peak_ws_bytes=last[0] if last else None, peak_commit_bytes=last[1] if last else None)

def sha256(path, chunk=1 << 22):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(chunk), b""): h.update(b)
    return h.hexdigest()

def write_json(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, indent=2, sort_keys=True); f.write("\n")

def make_test_png(path, w=512, h=512):
    """Deterministic synthetic test image (gradient, red disc, blue square, yellow bar); no external asset."""
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            r, g, b = x * 255 // (w - 1) // 2 + 40, y * 255 // (h - 1) // 2 + 40, 90
            if (x - 170) ** 2 + (y - 200) ** 2 < 80 ** 2: r, g, b = 220, 30, 30
            if 290 <= x < 440 and 270 <= y < 420: r, g, b = 30, 60, 220
            if 60 <= x < 450 and 440 <= y < 470: r, g, b = 240, 220, 40
            row += bytes((r, g, b))
        rows.append(bytes(row))
    def chunk(t, d): c = struct.pack(">I", len(d)) + t + d; return c + struct.pack(">I", zlib.crc32(t + d))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + \
          chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f: f.write(png)
