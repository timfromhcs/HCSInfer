#!/usr/bin/env python3
"""HCSInfer stress / limit harness (plan F2 skeleton, built on the F1 overload controller).

Ramps context length, image size and client concurrency step by step against a RUNNING `HCSInfer serve`,
polls memory through the server's overload controller (GET /hcs/v1/runtime/overload) and ABORTS the ramp the
moment any abort condition holds:

  free_ram_below_floor     free physical RAM < --floor-mb
  commit_above_limit       commit charge > --commit-pct percent of the commit limit
  server_critical          the controller itself reports level "critical" (its watchdog shed the load)
  error_rate               (hard errors / requests) of a step > --max-error-rate
  latency_spike            step median latency > --spike-factor x the previous step AND > --spike-min-ms
  process_gone             --server-pid no longer exists, or the health route is unreachable repeatedly
  runtime_limit            total wall time > --max-runtime-s
  c_drive_growth           free space of the C: drive shrank by more than --c-growth-mb (nothing of HCS may
                           write there; see docs/build.md)
  no_memory_visibility     neither the controller nor the local OS probe can tell free RAM (never ramp blind)

Server backpressure (HTTP 429/503) is NOT an error: it is the controller doing its job. A step in which more
than --max-refused-rate of the requests were refused ends that axis with outcome "limit_reached".

On abort the harness (unless --no-shed-on-abort) calls the kill switch POST /hcs/v1/runtime/shed so the
machine is relieved, then writes the JSON report. Standard library only; run it isolated:
    python -I tools/stress/stress.py --help
The bearer token is read from the environment (--token-env, default HCS_API_TOKEN) and is never printed or
written to the report.
"""
from __future__ import annotations

import argparse
import ctypes
import datetime as _dt
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

HARNESS_VERSION = 1
REPORT_SCHEMA_VERSION = 1
MIB = 1 << 20

EXIT_OK = 0
EXIT_USAGE = 2
EXIT_ABORTED = 3
EXIT_UNREACHABLE = 4
EXIT_INTERNAL = 70


# --------------------------------------------------------------------------------------------------------
# Memory samples
# --------------------------------------------------------------------------------------------------------
@dataclass
class MemSample:
    t: float = 0.0
    source: str = "unknown"  # "controller" | "local" | "unknown"
    free_bytes: Optional[int] = None
    commit_fraction: Optional[float] = None
    level: Optional[str] = None  # controller pressure level
    in_flight: Optional[int] = None

    def to_json(self) -> dict:
        return {
            "t": round(self.t, 3),
            "source": self.source,
            "free_bytes": self.free_bytes if self.free_bytes is not None else "unknown",
            "commit_fraction": round(self.commit_fraction, 4) if self.commit_fraction is not None else "unknown",
            "level": self.level if self.level is not None else "unknown",
            "in_flight": self.in_flight if self.in_flight is not None else "unknown",
        }


def sample_from_overload_json(doc: dict, t: float) -> MemSample:
    """Pure: turn GET /hcs/v1/runtime/overload into a MemSample. Unknown stays None (never guessed)."""
    s = MemSample(t=t, source="controller")
    if not isinstance(doc, dict) or not doc.get("enabled", False):
        s.source = "unknown"
        return s
    sm = doc.get("sample", {})
    free = sm.get("free_physical_bytes")
    s.free_bytes = free if isinstance(free, int) else None
    cf = sm.get("commit_fraction")
    s.commit_fraction = float(cf) if isinstance(cf, (int, float)) else None
    s.level = doc.get("level")
    inf = doc.get("in_flight")
    s.in_flight = inf if isinstance(inf, int) else None
    return s


def local_sample(t: float) -> MemSample:
    """Local OS probe (Windows only, ctypes). Used when the controller is disabled or unreachable."""
    s = MemSample(t=t, source="unknown")
    if os.name != "nt":
        try:
            with open("/proc/meminfo", "r", encoding="ascii") as f:
                kv = {ln.split(":")[0]: int(ln.split()[1]) * 1024 for ln in f if ":" in ln}
            if "MemAvailable" in kv:
                s.source = "local"
                s.free_bytes = kv["MemAvailable"]
                if kv.get("CommitLimit"):
                    s.commit_fraction = kv.get("Committed_AS", 0) / kv["CommitLimit"]
        except OSError:
            pass
        return s

    class MEMORYSTATUSEX(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                    ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                    ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                    ("sullAvailExtendedVirtual", ctypes.c_ulonglong)]

    class PERFORMANCE_INFORMATION(ctypes.Structure):
        _fields_ = [("cb", ctypes.c_ulong), ("CommitTotal", ctypes.c_size_t), ("CommitLimit", ctypes.c_size_t),
                    ("CommitPeak", ctypes.c_size_t), ("PhysicalTotal", ctypes.c_size_t),
                    ("PhysicalAvailable", ctypes.c_size_t), ("SystemCache", ctypes.c_size_t),
                    ("KernelTotal", ctypes.c_size_t), ("KernelPaged", ctypes.c_size_t),
                    ("KernelNonpaged", ctypes.c_size_t), ("PageSize", ctypes.c_size_t),
                    ("HandleCount", ctypes.c_ulong), ("ProcessCount", ctypes.c_ulong),
                    ("ThreadCount", ctypes.c_ulong)]

    try:
        ms = MEMORYSTATUSEX()
        ms.dwLength = ctypes.sizeof(ms)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(ms)):
            s.source = "local"
            s.free_bytes = int(ms.ullAvailPhys)
        pi = PERFORMANCE_INFORMATION()
        pi.cb = ctypes.sizeof(pi)
        if ctypes.windll.psapi.GetPerformanceInfo(ctypes.byref(pi), pi.cb) and pi.CommitLimit:
            s.commit_fraction = pi.CommitTotal / pi.CommitLimit
    except (OSError, AttributeError):
        pass
    return s


def pid_alive(pid: int) -> bool:
    if os.name == "nt":
        try:
            k32 = ctypes.windll.kernel32
            h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
            if not h:
                return False
            try:
                code = ctypes.c_ulong()
                ok = k32.GetExitCodeProcess(h, ctypes.byref(code))
                return bool(ok) and code.value == 259  # STILL_ACTIVE
            finally:
                k32.CloseHandle(h)
        except (OSError, AttributeError):
            return True  # cannot tell: do not abort on a guess
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


# --------------------------------------------------------------------------------------------------------
# Abort logic (pure, unit-tested)
# --------------------------------------------------------------------------------------------------------
@dataclass
class Limits:
    floor_bytes: int = 2048 * MIB
    commit_fraction: float = 0.90
    max_error_rate: float = 0.5
    max_refused_rate: float = 0.5
    spike_factor: float = 8.0
    spike_min_ms: float = 5000.0
    max_runtime_s: float = 900.0
    c_growth_bytes: int = 512 * MIB


@dataclass
class Abort:
    reason: str
    detail: str

    def to_json(self) -> dict:
        return {"fired": True, "reason": self.reason, "detail": self.detail}


def check_memory(s: MemSample, lim: Limits) -> Optional[Abort]:
    """The memory-side abort conditions for one sample."""
    if s.free_bytes is None and s.commit_fraction is None:
        return Abort("no_memory_visibility", "neither free RAM nor the commit charge is known: refusing to ramp blind")
    if s.level == "critical":
        return Abort("server_critical", "the overload controller reports level 'critical' (its watchdog is shedding load)")
    if s.free_bytes is not None and s.free_bytes < lim.floor_bytes:
        return Abort("free_ram_below_floor",
                     f"free RAM {s.free_bytes // MIB} MiB < floor {lim.floor_bytes // MIB} MiB")
    if s.commit_fraction is not None and s.commit_fraction > lim.commit_fraction:
        return Abort("commit_above_limit",
                     f"commit charge {s.commit_fraction * 100:.1f}% > limit {lim.commit_fraction * 100:.1f}%")
    return None


def check_step(requests: int, errors: int, median_ms: Optional[float], prev_median_ms: Optional[float],
               lim: Limits) -> Optional[Abort]:
    """Step-level abort conditions (error rate, latency spike)."""
    if requests > 0 and errors / requests > lim.max_error_rate:
        return Abort("error_rate", f"{errors}/{requests} requests failed (limit {lim.max_error_rate:.0%})")
    if (median_ms is not None and prev_median_ms is not None and prev_median_ms > 0
            and median_ms > lim.spike_min_ms and median_ms > lim.spike_factor * prev_median_ms):
        return Abort("latency_spike",
                     f"median latency {median_ms:.0f} ms is {median_ms / prev_median_ms:.1f}x the previous step "
                     f"({prev_median_ms:.0f} ms)")
    return None


def check_runtime(elapsed_s: float, lim: Limits) -> Optional[Abort]:
    if elapsed_s > lim.max_runtime_s:
        return Abort("runtime_limit", f"{elapsed_s:.1f} s > limit {lim.max_runtime_s:.1f} s")
    return None


def check_c_drive(baseline_free: Optional[int], now_free: Optional[int], lim: Limits) -> Optional[Abort]:
    if baseline_free is None or now_free is None:
        return None
    grown = baseline_free - now_free
    if grown > lim.c_growth_bytes:
        return Abort("c_drive_growth", f"free space on C: shrank by {grown // MIB} MiB (limit {lim.c_growth_bytes // MIB} MiB)")
    return None


def classify(status: int) -> str:
    """ok | refused (429/503 backpressure) | error."""
    if 200 <= status < 300:
        return "ok"
    if status in (429, 503):
        return "refused"
    return "error"


def percentile(values: list[float], p: float) -> Optional[float]:
    if not values:
        return None
    v = sorted(values)
    k = max(0, min(len(v) - 1, int(round(p * (len(v) - 1)))))
    return v[k]


# --------------------------------------------------------------------------------------------------------
# HTTP client
# --------------------------------------------------------------------------------------------------------
class Client:
    def __init__(self, base_url: str, token: str, timeout: float):
        self.base = base_url.rstrip("/")
        self._token = token
        self.timeout = timeout

    def request(self, method: str, path: str, body: Optional[dict] = None, timeout: Optional[float] = None,
                auth: bool = True) -> tuple[int, dict[str, str], Any]:
        data = json.dumps(body).encode("utf-8") if body is not None else (b"" if method == "POST" else None)
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if body is not None or method == "POST":
            req.add_header("Content-Type", "application/json")
        if auth:
            req.add_header("Authorization", "Bearer " + self._token)
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as r:
                raw = r.read()
                return r.status, {k.lower(): v for k, v in r.headers.items()}, _maybe_json(raw)
        except urllib.error.HTTPError as e:
            raw = e.read()
            e.close()
            return e.code, {k.lower(): v for k, v in e.headers.items()}, _maybe_json(raw)
        except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as e:
            return 0, {}, {"transport_error": type(e).__name__}


def _maybe_json(raw: bytes) -> Any:
    try:
        return json.loads(raw.decode("utf-8")) if raw else None
    except (ValueError, UnicodeDecodeError):
        return None


# --------------------------------------------------------------------------------------------------------
# The run
# --------------------------------------------------------------------------------------------------------
@dataclass
class StepResult:
    axis: str
    value: int
    requests: int = 0
    ok: int = 0
    refused: int = 0
    errors: int = 0
    latencies_ms: list[float] = field(default_factory=list)
    status_counts: dict[str, int] = field(default_factory=dict)
    retry_after_seen: list[str] = field(default_factory=list)
    free_before: Optional[int] = None
    free_min_during: Optional[int] = None
    free_after: Optional[int] = None
    commit_max: Optional[float] = None
    outcome: str = "completed"  # completed | limit_reached | aborted

    def to_json(self) -> dict:
        med = statistics.median(self.latencies_ms) if self.latencies_ms else None
        return {
            "axis": self.axis, "value": self.value, "requests": self.requests, "ok": self.ok,
            "refused": self.refused, "errors": self.errors,
            "latency_ms_median": round(med, 1) if med is not None else "unknown",
            "latency_ms_p90": round(percentile(self.latencies_ms, 0.9), 1) if self.latencies_ms else "unknown",
            "status_counts": self.status_counts,
            "retry_after_seen": sorted(set(self.retry_after_seen)),
            "memory": {
                "free_bytes_before": _u(self.free_before), "free_bytes_min_during": _u(self.free_min_during),
                "free_bytes_after": _u(self.free_after),
                "commit_fraction_max": round(self.commit_max, 4) if self.commit_max is not None else "unknown",
            },
            "outcome": self.outcome,
        }


def _u(v: Optional[int]) -> Any:
    return v if v is not None else "unknown"


class Run:
    def __init__(self, client: Client, args: argparse.Namespace, lim: Limits,
                 mem_fn: Optional[Callable[[], MemSample]] = None):
        self.c = client
        self.a = args
        self.lim = lim
        self.t0 = time.monotonic()
        self.steps: list[StepResult] = []
        self.timeline: list[dict] = []
        self.abort: Optional[Abort] = None
        self.abort_event = threading.Event()
        self.abort_step: Optional[str] = None
        self.shed_report: Any = None
        self.unreachable = 0
        self._mem_fn = mem_fn or self._poll_memory
        self.c_baseline = _c_free(args.c_drive_path) if args.c_growth_mb > 0 else None
        self._lock = threading.Lock()

    # -- memory ----------------------------------------------------------------------------------------
    def _poll_memory(self) -> MemSample:
        t = time.monotonic() - self.t0
        st, _, doc = self.c.request("GET", "/hcs/v1/runtime/overload", timeout=5)
        if st == 200 and isinstance(doc, dict):
            s = sample_from_overload_json(doc, t)
            if s.source == "controller":
                return s
        if st == 0:
            self.unreachable += 1
        return local_sample(t)

    def sample(self) -> MemSample:
        s = self._mem_fn()
        with self._lock:
            if len(self.timeline) < 4000:
                self.timeline.append(s.to_json())
        return s

    def _trip(self, ab: Abort, where: str) -> None:
        first = False
        with self._lock:
            if self.abort is None:
                self.abort = ab
                self.abort_step = where
                first = True
        self.abort_event.set()
        # Relieve the machine immediately (the kill switch cancels the requests still in flight, so the step
        # can end quickly). Not when the server is gone: there is nobody to shed.
        if first and not self.a.no_shed_on_abort and ab.reason != "process_gone":
            st, _, doc = self.c.request("POST", "/hcs/v1/runtime/shed", timeout=30)
            self.shed_report = {"http_status": st, "report": doc}

    def guard(self, where: str) -> bool:
        """One full safety evaluation; returns True when the run must stop."""
        if self.abort_event.is_set():
            return True
        for ab in (check_runtime(time.monotonic() - self.t0, self.lim),
                   check_c_drive(self.c_baseline, _c_free(self.a.c_drive_path) if self.c_baseline is not None else None,
                                 self.lim)):
            if ab:
                self._trip(ab, where)
                return True
        if self.a.server_pid and not pid_alive(self.a.server_pid):
            self._trip(Abort("process_gone", f"server process {self.a.server_pid} no longer exists"), where)
            return True
        s = self.sample()
        if self.unreachable >= 3:
            self._trip(Abort("process_gone", "the server stopped answering (3 consecutive polls failed)"), where)
            return True
        ab = check_memory(s, self.lim)
        if ab:
            self._trip(ab, where)
            return True
        if s.source == "controller":
            self.unreachable = 0
        return False

    # -- requests --------------------------------------------------------------------------------------
    def _one(self, res: StepResult, method: str, path: str, body: dict) -> None:
        if self.abort_event.is_set():
            return
        t = time.monotonic()
        st, hdr, _ = self.c.request(method, path, body, timeout=self.a.request_timeout)
        ms = (time.monotonic() - t) * 1000.0
        kind = "error" if st == 0 else classify(st)
        with self._lock:
            res.requests += 1
            res.status_counts[str(st)] = res.status_counts.get(str(st), 0) + 1
            if kind == "ok":
                res.ok += 1
                res.latencies_ms.append(ms)
            elif kind == "refused":
                res.refused += 1
                if "retry-after" in hdr:
                    res.retry_after_seen.append(hdr["retry-after"])
            else:
                res.errors += 1

    def run_step(self, axis: str, value: int, jobs: list[tuple[str, str, dict]], parallel: int,
                 prev_median: Optional[float]) -> StepResult:
        res = StepResult(axis=axis, value=value)
        where = f"{axis}={value}"
        before = self.sample()
        res.free_before = before.free_bytes
        res.free_min_during = before.free_bytes
        res.commit_max = before.commit_fraction
        stop_mon = threading.Event()

        def monitor() -> None:
            while not stop_mon.wait(self.a.poll_interval):
                if self.guard(where):
                    return
                last = self.sample_cache
                if last is not None:
                    with self._lock:
                        if last.free_bytes is not None:
                            res.free_min_during = (last.free_bytes if res.free_min_during is None
                                                   else min(res.free_min_during, last.free_bytes))
                        if last.commit_fraction is not None:
                            res.commit_max = (last.commit_fraction if res.commit_max is None
                                              else max(res.commit_max, last.commit_fraction))

        self.sample_cache: Optional[MemSample] = before
        orig_sample = self._mem_fn

        def caching() -> MemSample:
            s = orig_sample()
            self.sample_cache = s
            return s

        self._mem_fn = caching
        mon = threading.Thread(target=monitor, daemon=True)
        mon.start()
        try:
            for i in range(0, len(jobs), parallel):
                if self.abort_event.is_set():
                    break
                batch = jobs[i:i + parallel]
                ths = [threading.Thread(target=self._one, args=(res, m, p, b)) for (m, p, b) in batch]
                for th in ths:
                    th.start()
                for th in ths:
                    th.join()
        finally:
            stop_mon.set()
            mon.join(timeout=5)
            self._mem_fn = orig_sample
        after = self.sample()
        res.free_after = after.free_bytes
        if after.free_bytes is not None:
            res.free_min_during = after.free_bytes if res.free_min_during is None else min(res.free_min_during, after.free_bytes)
        self.steps.append(res)
        if self.abort_event.is_set():
            res.outcome = "aborted"
            return res
        # post-step checks (memory once more with the final sample, then error rate / latency spike)
        ab = check_memory(after, self.lim)
        med = statistics.median(res.latencies_ms) if res.latencies_ms else None
        ab = ab or check_step(res.requests, res.errors, med, prev_median, self.lim)
        if ab:
            self._trip(ab, where)
            res.outcome = "aborted"
        elif res.requests and res.refused / res.requests > self.lim.max_refused_rate:
            res.outcome = "limit_reached"
        return res

    # -- axes ------------------------------------------------------------------------------------------
    def chat_body(self, tokens: int) -> dict:
        return {"model": self.a.model, "stream": False, "max_tokens": self.a.max_tokens,
                "messages": [{"role": "user", "content": ("lorem " * max(1, tokens)).strip()}]}

    def axis_context(self) -> None:
        prev: Optional[float] = None
        for n in self.a.context_steps:
            if self.guard(f"context={n}"):
                return
            jobs = [("POST", "/v1/chat/completions", self.chat_body(n)) for _ in range(self.a.reps)]
            r = self.run_step("context", n, jobs, 1, prev)
            if r.outcome != "completed":
                return
            prev = statistics.median(r.latencies_ms) if r.latencies_ms else prev

    def axis_concurrency(self) -> None:
        prev: Optional[float] = None
        for n in self.a.concurrency_steps:
            if self.guard(f"concurrency={n}"):
                return
            jobs = [("POST", "/v1/chat/completions", self.chat_body(self.a.concurrency_prompt_tokens))
                    for _ in range(n * self.a.reps)]
            r = self.run_step("concurrency", n, jobs, n, prev)
            if r.outcome != "completed":
                return
            prev = statistics.median(r.latencies_ms) if r.latencies_ms else prev

    def axis_image(self) -> None:
        prev: Optional[float] = None
        for px in self.a.image_steps:
            if self.guard(f"image={px}"):
                return
            body = {"model": self.a.image_model or self.a.model, "prompt": "a small red apple on a table",
                    "size": f"{px}x{px}", "n": 1, "steps": self.a.image_steps_count}
            jobs = [("POST", "/v1/images/generations", body) for _ in range(self.a.reps)]
            r = self.run_step("image", px, jobs, 1, prev)
            if r.outcome != "completed":
                return
            prev = statistics.median(r.latencies_ms) if r.latencies_ms else prev

    def execute(self) -> None:
        axes = {"context": self.axis_context, "concurrency": self.axis_concurrency, "image": self.axis_image}
        for name in self.a.axes:
            if self.abort_event.is_set():
                break
            axes[name]()


def _c_free(path: str) -> Optional[int]:
    try:
        return shutil.disk_usage(path).free
    except OSError:
        return None


# --------------------------------------------------------------------------------------------------------
# Report
# --------------------------------------------------------------------------------------------------------
def git_commit() -> str:
    try:
        out = subprocess.run(["git", "rev-parse", "--short=12", "HEAD"], capture_output=True, text=True, timeout=10,
                             cwd=os.path.dirname(os.path.abspath(__file__)))
        return out.stdout.strip() or "unknown"
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def build_report(run: Run, args: argparse.Namespace, lim: Limits, server_runtime: Any, status: str) -> dict:
    return {
        "schema_version": REPORT_SCHEMA_VERSION,
        "harness_version": HARNESS_VERSION,
        "timestamp_utc": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "git_commit": git_commit(),
        "mode": "dry" if args.dry else "ramp",
        "result_status": status,  # PASS | ABORTED | LIMIT_REACHED | FAIL
        "os": platform.platform(),
        "python": platform.python_version(),
        "server": {"base_url": args.base_url, "model": args.model, "runtime": server_runtime},
        "config": {
            "axes": args.axes, "context_steps": args.context_steps, "concurrency_steps": args.concurrency_steps,
            "image_steps": args.image_steps, "reps": args.reps, "max_tokens": args.max_tokens,
            "limits": {"floor_mib": lim.floor_bytes // MIB, "commit_pct": lim.commit_fraction * 100,
                       "max_error_rate": lim.max_error_rate, "max_refused_rate": lim.max_refused_rate,
                       "spike_factor": lim.spike_factor, "spike_min_ms": lim.spike_min_ms,
                       "max_runtime_s": lim.max_runtime_s, "c_growth_mib": lim.c_growth_bytes // MIB},
        },
        "abort": run.abort.to_json() | {"at": run.abort_step} if run.abort else {"fired": False},
        "shed_on_abort": run.shed_report,
        "duration_s": round(time.monotonic() - run.t0, 2),
        "steps": [s.to_json() for s in run.steps],
        "memory_timeline": _downsample(run.timeline, 600),
    }


def _downsample(items: list[dict], n: int) -> list[dict]:
    if len(items) <= n:
        return items
    stride = len(items) / n
    return [items[int(i * stride)] for i in range(n)]


# --------------------------------------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------------------------------------
def _ints(text: str) -> list[int]:
    return [int(x) for x in text.split(",") if x.strip()]


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="HCSInfer stress/limit harness (see the module docstring).")
    p.add_argument("--base-url", default="http://127.0.0.1:8765")
    p.add_argument("--token-env", default="HCS_API_TOKEN", help="environment variable holding the bearer token")
    p.add_argument("--model", required=True, help="registered model id used for chat (and images unless --image-model)")
    p.add_argument("--image-model", default="")
    p.add_argument("--axes", default="context,concurrency", help="comma list of: context, concurrency, image")
    p.add_argument("--dry", action="store_true", help="tiny steps (smoke run); explicit --*-steps still override")
    p.add_argument("--context-steps", default=None, help="approx prompt tokens per step, e.g. 64,256,1024,4096")
    p.add_argument("--concurrency-steps", default=None, help="parallel clients per step, e.g. 1,2,4,8")
    p.add_argument("--image-steps", default=None, help="square image edge per step, e.g. 128,256,512")
    p.add_argument("--image-steps-count", type=int, default=4, help="diffusion steps per image request")
    p.add_argument("--concurrency-prompt-tokens", type=int, default=64)
    p.add_argument("--reps", type=int, default=None, help="requests per step (per client for concurrency)")
    p.add_argument("--max-tokens", type=int, default=8)
    p.add_argument("--request-timeout", type=float, default=180.0)
    p.add_argument("--poll-interval", type=float, default=0.5)
    p.add_argument("--floor-mb", type=int, default=2048, help="abort when free RAM drops below this")
    p.add_argument("--commit-pct", type=float, default=90.0)
    p.add_argument("--max-error-rate", type=float, default=0.5)
    p.add_argument("--max-refused-rate", type=float, default=0.5)
    p.add_argument("--spike-factor", type=float, default=8.0)
    p.add_argument("--spike-min-ms", type=float, default=5000.0)
    p.add_argument("--max-runtime-s", type=float, default=900.0)
    p.add_argument("--c-growth-mb", type=int, default=512, help="abort when C: free space shrinks by this much; 0 = off")
    p.add_argument("--c-drive-path", default="C:\\" if os.name == "nt" else "/")
    p.add_argument("--server-pid", type=int, default=0, help="abort when this process disappears")
    p.add_argument("--no-shed-on-abort", action="store_true")
    p.add_argument("--report", default="stress-report.json")
    a = p.parse_args(argv)
    a.axes = [x.strip() for x in a.axes.split(",") if x.strip()]
    for x in a.axes:
        if x not in ("context", "concurrency", "image"):
            p.error(f"unknown axis {x!r}")
    d = a.dry
    a.context_steps = _ints(a.context_steps) if a.context_steps else ([16, 64, 256] if d else [64, 256, 1024, 2048, 4096])
    a.concurrency_steps = _ints(a.concurrency_steps) if a.concurrency_steps else ([1, 2] if d else [1, 2, 4, 8])
    a.image_steps = _ints(a.image_steps) if a.image_steps else ([128, 256] if d else [128, 256, 384, 512])
    a.reps = a.reps if a.reps is not None else (1 if d else 2)
    if a.reps < 1 or a.max_tokens < 1:
        p.error("--reps and --max-tokens must be >= 1")
    return a


def main(argv: Optional[list[str]] = None) -> int:
    a = parse_args(sys.argv[1:] if argv is None else argv)
    token = os.environ.get(a.token_env, "")
    if not token:
        print(f"error: environment variable {a.token_env} is empty (the token is never taken from argv)",
              file=sys.stderr)
        return EXIT_USAGE
    lim = Limits(floor_bytes=a.floor_mb * MIB, commit_fraction=a.commit_pct / 100.0,
                 max_error_rate=a.max_error_rate, max_refused_rate=a.max_refused_rate,
                 spike_factor=a.spike_factor, spike_min_ms=a.spike_min_ms, max_runtime_s=a.max_runtime_s,
                 c_growth_bytes=a.c_growth_mb * MIB)
    client = Client(a.base_url, token, a.request_timeout)
    st, _, health = client.request("GET", "/hcs/v1/health", auth=False, timeout=10)
    if st != 200:
        print(f"error: server not reachable at {a.base_url} (health status {st})", file=sys.stderr)
        return EXIT_UNREACHABLE
    _, _, runtime = client.request("GET", "/hcs/v1/runtime", timeout=10)
    run = Run(client, a, lim)
    status = "FAIL"
    try:
        run.execute()
        if run.abort:
            status = "ABORTED"
        elif any(s.outcome == "limit_reached" for s in run.steps):
            status = "LIMIT_REACHED"
        else:
            status = "PASS"
    except Exception as e:  # noqa: BLE001 - the report must still be written
        print(f"error: harness failure: {type(e).__name__}: {e}", file=sys.stderr)
        run.abort = run.abort or Abort("harness_error", f"{type(e).__name__}: {e}")
    finally:
        rep = build_report(run, a, lim, runtime, status)
        with open(a.report, "w", encoding="utf-8") as f:
            json.dump(rep, f, indent=2)
    summary = f"{status}: {len(run.steps)} steps, {rep['duration_s']} s, report {a.report}"
    if run.abort:
        summary += f" | ABORT {run.abort.reason}: {run.abort.detail} (at {run.abort_step})"
    print(summary)
    if status == "FAIL":
        return EXIT_INTERNAL
    return EXIT_ABORTED if status == "ABORTED" else EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
