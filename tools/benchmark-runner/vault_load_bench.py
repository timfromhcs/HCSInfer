"""Measures end-to-end model load through a REAL `HCSInfer serve` process: engine-default load versus the opt-in
Model Vault stage (`serve --vault-prefetch`), cold and warm, N repetitions each, interleaved.

  python -I vault_load_bench.py --exe HCSInfer.exe --model-file X.gguf --out result.json [--runs 3]
         [--state-dir DIR] [--buffers 4] [--chunk-mib 16] [--label ssd-d]

What is measured per run (all wall clock, perf_counter):
  startup_s     process start -> GET /hcs/v1/health == 200
  load_s        POST /hcs/v1/models/<id>/load -> job "succeeded" (includes the vault stage when enabled)
  vault_s       the vault stage alone (server-reported wall time of the bounded sequential read + SHA-256)
  peak_*        server process PeakWorkingSetSize / PeakPagefileUsage (GetProcessMemoryInfo)
"Cold" means the model file was evicted from the Windows file cache right before the load with cache_evict.py
(no administrator rights; its effect is verified by a read-speed check and recorded). If eviction cannot be
verified the cold runs are reported with "cold_verified": false and must not be quoted as cold.
Nothing is written next to the model; the only files created live under --state-dir (registry/logs).
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import glob
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cache_evict  # noqa: E402

TOKEN = "vault-bench-token-0123456789abcdef"


class PMC(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD), ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t)]


class IOC(ctypes.Structure):
    _fields_ = [("ReadOperationCount", ctypes.c_ulonglong), ("WriteOperationCount", ctypes.c_ulonglong),
                ("OtherOperationCount", ctypes.c_ulonglong), ("ReadTransferCount", ctypes.c_ulonglong),
                ("WriteTransferCount", ctypes.c_ulonglong), ("OtherTransferCount", ctypes.c_ulonglong)]


def io_counters(pid):
    """Bytes the process read/wrote in total (GetProcessIoCounters). A vault stage that staged a decompressed
    copy somewhere would show up as write bytes of the order of the model size."""
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = wt.HANDLE
    h = k32.OpenProcess(0x1000, False, pid)
    if not h:
        return {}
    try:
        c = IOC()
        k32.GetProcessIoCounters.argtypes = [wt.HANDLE, ctypes.POINTER(IOC)]
        if not k32.GetProcessIoCounters(h, ctypes.byref(c)):
            return {}
        return {"io_read_bytes": c.ReadTransferCount, "io_write_bytes": c.WriteTransferCount}
    finally:
        k32.CloseHandle(h)


def peak_memory(pid):
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    k32.OpenProcess.restype = wt.HANDLE
    h = k32.OpenProcess(0x1000 | 0x0010, False, pid)  # QUERY_LIMITED_INFORMATION | VM_READ
    if not h:
        return {}
    try:
        pmc = PMC()
        pmc.cb = ctypes.sizeof(PMC)
        psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.POINTER(PMC), wt.DWORD]
        if not psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb):
            return {}
        return {"peak_working_set_bytes": pmc.PeakWorkingSetSize, "peak_pagefile_bytes": pmc.PeakPagefileUsage}
    finally:
        k32.CloseHandle(h)


def http(port, method, path, body=None, token=True, timeout=30):
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method,
                                 data=(body if body is not None else (b"{}" if method == "POST" else None)))
    if token:
        req.add_header("Authorization", f"Bearer {TOKEN}")
    if method == "POST":
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"null")
        except Exception:
            return e.code, None


def free_port():
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def one_run(exe, state_dir, model_dir, model_id, model_file, vault, cold, args):
    port = free_port()
    cmd = [exe, "--state-dir", state_dir, "serve", "--port", str(port), "--token", TOKEN, "--model-root", model_dir]
    if vault:
        cmd += ["--vault-prefetch", "--vault-buffers", str(args.buffers), "--vault-chunk-mib", str(args.chunk_mib)]
    log = open(os.path.join(state_dir, "serve.log"), "ab")
    t0 = time.perf_counter()
    p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.PIPE)
    listening = {}

    def pump():
        for raw in iter(p.stderr.readline, b""):
            log.write(raw)
            if b"listening on http://" in raw and "t" not in listening:
                listening["t"] = time.perf_counter() - t0

    pump_thread = threading.Thread(target=pump, daemon=True)
    pump_thread.start()
    rec = {"mode": "vault-prefetch" if vault else "engine-default", "cache": "cold" if cold else "warm"}
    try:
        # startup = process start -> the server announces it is listening (the socket is bound and accepting).
        # Polling /health instead would add Windows' ~0.5 s retry quantum for a refused loopback connect.
        while time.perf_counter() - t0 < 60 and "t" not in listening:
            time.sleep(0.002)
        startup = listening.get("t")
        if startup is None:
            rec["error"] = "server did not announce a listening socket in 60 s"
            return rec
        st, _ = http(port, "GET", "/hcs/v1/health", token=False, timeout=5)
        rec["first_health_ok"] = st == 200
        rec["startup_s"] = round(startup, 3)
        # The engine loads a sibling mmproj together with a vision-capable text model: treat it like the model.
        files = [model_file] + sorted(glob.glob(os.path.join(model_dir, "mmproj*.gguf")))
        rec["files"] = [os.path.basename(f) for f in files]
        if cold:
            rec["evict_open_ok"] = all([cache_evict.evict(f) for f in files])
        else:
            for f in files:
                cache_evict.read_prefix_mib_s(f, 100000)  # whole file into the cache first
        t1 = time.perf_counter()
        st, j = http(port, "POST", f"/hcs/v1/models/{model_id}/load")
        if st != 202:
            rec["error"] = f"load answered HTTP {st}: {j}"
            return rec
        job = j["job_id"]
        state = None
        while time.perf_counter() - t1 < args.timeout_s:
            _, js = http(port, "GET", f"/hcs/v1/jobs/{job}")
            state = js["state"]
            if state in ("succeeded", "failed", "cancelled"):
                break
            time.sleep(0.05)
        rec["load_s"] = round(time.perf_counter() - t1, 3)
        rec["state"] = state
        if state != "succeeded":
            rec["error"] = js.get("error")
            return rec
        _, rt = http(port, "GET", "/hcs/v1/runtime")
        if rt.get("vault"):
            m = rt["vault"].get("model", {})
            rec["vault_s"] = round(m.get("wall_seconds", 0), 3)
            rec["vault_mib_s"] = round(m.get("mib_per_second", 0), 1)
            rec["vault_sha256"] = m.get("sha256")
            rec["vault_verified_against_manifest"] = m.get("verified_against_manifest")
            rec["vault_buffer_bytes"] = m.get("buffer_bytes")
        em = (rt.get("text_engine") or {}).get("metrics") or {}
        rec["engine_load_ms"] = em.get("load_ms")
        rec["engine_backend"] = em.get("backend")
        rec.update(peak_memory(p.pid))
        rec.update(io_counters(p.pid))
        http(port, "POST", f"/hcs/v1/models/{model_id}/unload")
        return rec
    finally:
        p.terminate()
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            p.kill()
        pump_thread.join(5)
        log.close()


def summarize(runs, key):
    v = [r[key] for r in runs if key in r and isinstance(r[key], (int, float))]
    if not v:
        return None
    v.sort()
    return {"n": len(v), "median": statistics.median(v), "min": v[0], "max": v[-1],
            "p90": v[min(len(v) - 1, int(round(0.9 * (len(v) - 1))))]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--model-file", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--state-dir")
    ap.add_argument("--buffers", type=int, default=4)
    ap.add_argument("--chunk-mib", type=int, default=16)
    ap.add_argument("--timeout-s", type=int, default=600)
    ap.add_argument("--label", default="")
    ap.add_argument("--skip-cold", action="store_true")
    args = ap.parse_args()
    model_file = os.path.abspath(args.model_file)
    model_dir = os.path.dirname(model_file)
    own_state = args.state_dir is None
    state_dir = args.state_dir or tempfile.mkdtemp(prefix="vault-bench-")  # TEMP must already be off drive C:
    os.makedirs(state_dir, exist_ok=True)
    try:
        sc = subprocess.run([args.exe, "--state-dir", state_dir, "models", "scan", model_dir, "--json"],
                            capture_output=True, text=True)
        if sc.returncode != 0:
            print(sc.stderr, file=sys.stderr)
            return 2
        lst = subprocess.run([args.exe, "--state-dir", state_dir, "models", "list", "--json"],
                             capture_output=True, text=True)
        models = [m for m in json.loads(lst.stdout)["models"] if os.path.samefile(m["path"], model_file)]
        if not models:
            print("model not found in registry after scan", file=sys.stderr)
            return 2
        model_id = models[0]["id"]
        ev = cache_evict.verify(model_file, 256)
        runs = []
        order = []
        for i in range(args.runs):
            seq = [(False, True), (True, True), (False, False), (True, False)]  # (vault, cold)
            if i % 2:
                seq = seq[::-1]  # alternate direction to cancel monotonic drift
            for vault, cold in seq:
                if cold and (args.skip_cold or not ev["effective"]):
                    continue
                order.append((vault, cold))
                r = one_run(args.exe, state_dir, model_dir, model_id, model_file, vault, cold, args)
                r["repetition"] = i + 1
                runs.append(r)
                print(json.dumps(r), flush=True)
        groups = {}
        for r in runs:
            groups.setdefault((r["mode"], r["cache"]), []).append(r)
        summary = [{"mode": k[0], "cache": k[1], "load_s": summarize(v, "load_s"), "startup_s": summarize(v, "startup_s"),
                    "vault_s": summarize(v, "vault_s"), "peak_working_set_bytes": summarize(v, "peak_working_set_bytes"),
                    "peak_pagefile_bytes": summarize(v, "peak_pagefile_bytes"),
                    "io_write_bytes": summarize(v, "io_write_bytes"), "io_read_bytes": summarize(v, "io_read_bytes"),
                    "failures": sum(1 for x in v if "error" in x)} for k, v in sorted(groups.items())]
        out = {"schema_version": 1, "tool": "vault_load_bench.py", "label": args.label,
               "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
               "os": platform.platform(), "model_file": os.path.basename(model_file),
               "model_size_bytes": os.path.getsize(model_file), "model_id": model_id,
               "vault_buffers": args.buffers, "vault_chunk_mib": args.chunk_mib, "runs_per_cell": args.runs,
               "cold_verified": ev["effective"], "cold_evict_check": ev,
               "exe": os.path.basename(args.exe), "summary": summary, "runs": runs}
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=2)
        return 0
    finally:
        if own_state:
            shutil.rmtree(state_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
