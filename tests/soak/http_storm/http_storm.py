"""HTTP reliability storm against a real `HCSInfer serve` (real LFM3-Omni-2B; no mocks).

Phases (AGENTS.md 16.1G / 17.7): client-disconnect storm on streaming and non-streaming chat, slow/idle
connection flood, model unload/load while clients vanish, hard-kill of the server while a stream is running
followed by a restart (crash recovery), and an attempt at a graceful Ctrl+C shutdown while generating.
The server process is sampled (private bytes, handles, threads) before/after every phase; growth beyond the
documented limits (docs/soak.md) fails the run.

Run:  python -I http_storm.py --exe <HCSInfer.exe> --model-root D:\\HCSModels --out <dir> [--rounds 40]
Exit: 0 PASS, 1 FAIL, 77 BLOCKED (prerequisite missing). Stdlib only.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import http.client
import json
import os
import queue
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

CHECKS = []
SAMPLES = []


def check(name, cond, detail=""):
    CHECKS.append({"check": name, "pass": bool(cond), "detail": str(detail)[:500]})
    print(("PASS  " if cond else "FAIL  ") + name + ((" -- " + str(detail)[:300]) if not cond and detail else ""), flush=True)
    return bool(cond)


# ---- process sampling (ctypes, no psutil) -------------------------------------------------------------
class PMC(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD), ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t), ("PrivateUsage", ctypes.c_size_t)]


k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.GetProcessHandleCount.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.POINTER(PMC), wt.DWORD]


class TH(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG),
                ("dwFlags", wt.DWORD)]


def thread_count(pid):
    snap = k32.CreateToolhelp32Snapshot(0x4, 0)
    n = 0
    te = TH()
    te.dwSize = ctypes.sizeof(te)
    if snap and k32.Thread32First(snap, ctypes.byref(te)):
        while True:
            if te.th32OwnerProcessID == pid:
                n += 1
            if not k32.Thread32Next(snap, ctypes.byref(te)):
                break
    k32.CloseHandle(snap)
    return n


def proc_sample(pid, label):
    h = k32.OpenProcess(0x1000 | 0x0010, False, pid)  # QUERY_LIMITED_INFORMATION | VM_READ
    if not h:
        h = k32.OpenProcess(0x0400 | 0x0010, False, pid)
    pmc = PMC()
    pmc.cb = ctypes.sizeof(pmc)
    handles = wt.DWORD(0)
    ok = h and psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb)
    k32.GetProcessHandleCount(h, ctypes.byref(handles))
    if h:
        k32.CloseHandle(h)
    s = {"label": label, "t": time.time(), "private_bytes": int(pmc.PrivateUsage) if ok else None,
         "working_set": int(pmc.WorkingSetSize) if ok else None, "handles": int(handles.value),
         "threads": thread_count(pid)}
    SAMPLES.append(s)
    return s


# ---- server ---------------------------------------------------------------------------------------------
class Server:
    def __init__(self, exe, model_root, state_dir, extra=()):
        self.proc = subprocess.Popen([exe, "--state-dir", state_dir, "serve", "--port", "0", "--model-root",
                                      model_root, "--timeout-s", "600"] + list(extra),
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
        self.q = queue.Queue()
        self.lines = []
        threading.Thread(target=self._rd, daemon=True).start()
        self.port = self.token = None
        t0 = time.time()
        while time.time() - t0 < 60 and (self.port is None or self.token is None):
            try:
                line = self.q.get(timeout=1)
            except queue.Empty:
                continue
            if line is None:
                break
            self.lines.append(line)
            if "listening on http://" in line:
                self.port = int(line.rsplit(":", 1)[1])
            if line.startswith("HCSInfer API token: "):
                self.token = line.split(": ", 1)[1].strip()

    def _rd(self):
        for line in iter(self.proc.stderr.readline, ""):
            self.q.put(line.rstrip("\r\n"))
            self.lines.append(line.rstrip("\r\n"))
        self.q.put(None)

    def alive(self):
        return self.proc.poll() is None

    def req(self, method, path, body=None, timeout=300, auth=True):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        h = {"Authorization": "Bearer " + self.token} if auth else {}
        data = None
        if body is not None:
            data = json.dumps(body)
            h["Content-Type"] = "application/json"
        c.request(method, path, body=data, headers=h)
        r = c.getresponse()
        txt = r.read().decode("utf-8", "replace")
        c.close()
        try:
            return r.status, json.loads(txt) if txt else None
        except ValueError:
            return r.status, {"_raw": txt[:300]}

    def raw_post(self, path, body, rst=False):
        """POST on a raw socket, read the response headers (and first body bytes), return the socket."""
        s = socket.create_connection(("127.0.0.1", self.port), timeout=120)
        if rst:  # abortive close (TCP RST) instead of FIN
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        data = json.dumps(body).encode()
        s.sendall(("POST %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nAuthorization: Bearer %s\r\n"
                   "Content-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"
                   % (path, self.port, self.token, len(data))).encode() + data)
        return s

    def stop(self):
        if self.alive():
            self.proc.kill()
        try:
            self.proc.wait(timeout=30)
        except Exception:
            pass


def wait_health(srv, secs=60):
    t0 = time.time()
    while time.time() - t0 < secs:
        try:
            c, j = srv.req("GET", "/hcs/v1/health", auth=False, timeout=5)
            if c == 200:
                return time.time() - t0
        except Exception:
            pass
        time.sleep(0.25)
    return None


def chat_body(model, max_tokens, stream, content="Write a long story about a lighthouse keeper and a storm."):
    return {"model": model, "messages": [{"role": "user", "content": content}], "max_tokens": max_tokens,
            "stream": stream, "temperature": 0.7, "seed": 11}


def normal_chat(srv, model, tokens=16):
    t0 = time.time()
    c, j = srv.req("POST", "/v1/chat/completions", chat_body(model, tokens, False), timeout=300)
    txt = ""
    try:
        txt = j["choices"][0]["message"]["content"]
    except Exception:
        pass
    return c, txt, time.time() - t0


def idle_wait(srv, model, secs=120):
    """Polls until a short chat is served promptly again (i.e. no abandoned job still occupies the engine)."""
    t0 = time.time()
    while time.time() - t0 < secs:
        c, txt, dt = normal_chat(srv, model, 4)
        if c == 200 and txt and dt < 20:
            return True, dt, time.time() - t0
        time.sleep(1)
    return False, None, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--model-root", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--rounds", type=int, default=20)
    ap.add_argument("--max-private-growth-mb", type=float, default=192)
    ap.add_argument("--max-handle-growth", type=int, default=48)
    ap.add_argument("--max-thread-growth", type=int, default=8)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if not os.path.exists(a.exe) or not os.path.isdir(a.model_root):
        print("BLOCKED: exe or model root missing")
        return 77

    state = tempfile.mkdtemp(prefix="hcs-storm-state-")
    # `serve` only loads the persisted (read-only) registry; populate it the way a user does: `models scan`.
    scan = subprocess.run([a.exe, "--state-dir", state, "models", "scan", a.model_root], capture_output=True,
                          text=True, encoding="utf-8", timeout=300)
    if scan.returncode != 0:
        print("FAIL  models scan before serve: rc=%d %s" % (scan.returncode, scan.stderr[-300:]))
        return 1
    srv = Server(a.exe, a.model_root, state)
    if not check("serve starts, prints port and token", srv.port and srv.token, srv.lines[-3:]):
        srv.stop()
        return 1
    try:
        code, models = srv.req("GET", "/v1/models")
        ids = [m["id"] for m in (models or {}).get("data", [])]
        model = next((i for i in ids if "lfm3-omni-2b-q8_0" in i and "mmproj" not in i), None)
        if not check("LFM3-Omni-2B Q8_0 is listed", model, ids[:10]):
            return 1
        pid = srv.proc.pid
        c, txt, dt = normal_chat(srv, model, 16)  # loads the model
        check("baseline chat (model auto-load) works", c == 200 and txt, (c, txt[:80], dt))
        base = proc_sample(pid, "after-first-load")

        # ---- Phase A: streaming client-disconnect storm (FIN and RST) ---------------------------------
        print("== A: streaming disconnect storm ==", flush=True)
        a_fail = 0
        for i in range(a.rounds):
            try:
                s = srv.raw_post("/v1/chat/completions", chat_body(model, 512, True), rst=(i % 2 == 1))
                s.settimeout(60)
                got = b""
                while b"data:" not in got and len(got) < 65536:
                    d = s.recv(4096)
                    if not d:
                        break
                    got += d
                time.sleep((i * 53 % 300) / 1000.0)  # drop at varying points of the stream
                s.close()
            except Exception as e:
                a_fail += 1
                print("  round", i, "client error", e)
            if not srv.alive():
                break
            if i % 10 == 9:
                proc_sample(pid, "A-round-%d" % (i + 1))
        check("A: server survived the streaming disconnect storm", srv.alive(), srv.lines[-5:])
        ok, dt, waited = idle_wait(srv, model)
        check("A: engine is free again after the storm (short chat promptly served)", ok, (dt, waited))
        proc_sample(pid, "after-A")

        # ---- Phase B: abandoned non-streaming requests ------------------------------------------------
        print("== B: abandoned non-streaming requests ==", flush=True)
        for i in range(max(5, a.rounds // 2)):
            try:
                s = srv.raw_post("/v1/chat/completions", chat_body(model, 512, False), rst=(i % 2 == 0))
                time.sleep(0.3 + (i % 5) * 0.2)
                s.close()
            except Exception as e:
                print("  round", i, "client error", e)
        check("B: server alive after abandoned non-streaming requests", srv.alive())
        ok, dt, waited = idle_wait(srv, model, 180)
        check("B: abandoned requests were cancelled (engine free, not stuck generating 512 tokens each)", ok,
              (dt, waited))
        proc_sample(pid, "after-B")

        # ---- Phase C: connection flood (idle / half-written requests) ---------------------------------
        print("== C: idle and half-written connection flood ==", flush=True)
        socks = []
        for i in range(64):
            try:
                s = socket.create_connection(("127.0.0.1", srv.port), timeout=5)
                if i % 2:
                    s.sendall(b"POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 100000\r\n\r\n{")
                socks.append(s)
            except Exception:
                break
        t0 = time.time()
        served = None
        try:
            c, j = srv.req("GET", "/hcs/v1/health", auth=False, timeout=30)
            served = (c, time.time() - t0)
        except Exception as e:
            served = ("error", str(e))
        for s in socks:
            try:
                s.close()
            except Exception:
                pass
        check("C: health still answers while ~64 idle/half-open connections are held", served and served[0] == 200,
              served)
        check("C: server alive after the flood", srv.alive())
        w = wait_health(srv, 30)
        check("C: health recovers promptly after the flood is released", w is not None and w < 15, w)
        proc_sample(pid, "after-C")

        # ---- Phase D: unload/load churn while clients vanish -----------------------------------------
        print("== D: model load/unload churn with vanishing clients ==", flush=True)
        d_ok = True
        for i in range(max(3, a.rounds // 5)):
            try:
                s = srv.raw_post("/v1/chat/completions", chat_body(model, 256, True))
                s.settimeout(60)
                s.recv(2048)
                c1, _ = srv.req("POST", "/hcs/v1/models/%s/unload" % model, {}, timeout=120)  # cancels generation
                s.close()
                c2, _ = srv.req("POST", "/hcs/v1/models/%s/load" % model, {}, timeout=300)
                if c1 not in (200, 202, 409) or c2 not in (200, 202, 409):
                    d_ok = False
                    print("  unexpected statuses", c1, c2)
            except Exception as e:
                d_ok = False
                print("  D error", e)
        check("D: unload during a live stream / reload churn gave only clean statuses", d_ok and srv.alive())
        ok, dt, waited = idle_wait(srv, model, 300)
        check("D: chat works after churn", ok, (dt, waited))
        end = proc_sample(pid, "after-D")

        # ---- leak verdict for the server process -------------------------------------------------------
        dp = (end["private_bytes"] - base["private_bytes"]) / 1048576.0 if base["private_bytes"] else None
        dh = end["handles"] - base["handles"]
        dt_ = end["threads"] - base["threads"]
        check("server private-bytes growth within limit (%.0f MB)" % a.max_private_growth_mb,
              dp is not None and dp <= a.max_private_growth_mb, dp)
        check("server handle growth within limit (%d)" % a.max_handle_growth, dh <= a.max_handle_growth, dh)
        check("server thread growth within limit (%d)" % a.max_thread_growth, dt_ <= a.max_thread_growth, dt_)

        # ---- Phase E: hard kill while generating, then restart (crash recovery) ----------------------
        print("== E: hard kill during generation, restart ==", flush=True)
        s = srv.raw_post("/v1/chat/completions", chat_body(model, 512, True))
        s.settimeout(60)
        s.recv(2048)
        srv.proc.kill()
        srv.proc.wait(timeout=30)
        try:
            s.close()
        except Exception:
            pass
        srv2 = Server(a.exe, a.model_root, state)  # same state dir: registry/settings must still be readable
        try:
            check("E: restart after hard kill (same state dir)", srv2.port and srv2.token, srv2.lines[-4:])
            if srv2.port:
                c, txt, dt = normal_chat(srv2, model, 16)
                check("E: chat works after crash recovery", c == 200 and txt, (c, txt[:60], dt))

                # ---- Phase F: graceful Ctrl+C while generating -------------------------------------------
                print("== F: Ctrl+C during generation ==", flush=True)
                s = srv2.raw_post("/v1/chat/completions", chat_body(model, 1024, True))
                s.settimeout(60)
                s.recv(2048)
                prev = signal.signal(signal.SIGINT, lambda *_: None)
                sent = True
                try:
                    os.kill(0, signal.CTRL_C_EVENT)
                except Exception as e:
                    sent = False
                    print("  CTRL_C_EVENT not deliverable here:", e)
                if sent:
                    t0 = time.time()
                    try:
                        rc = srv2.proc.wait(timeout=60)
                        check("F: server exits on Ctrl+C while generating (rc=%s, %.1fs)" % (rc, time.time() - t0),
                              rc is not None and time.time() - t0 < 60, rc)
                    except subprocess.TimeoutExpired:
                        check("F: server exits on Ctrl+C while generating", False, "still running after 60 s")
                else:
                    CHECKS.append({"check": "F: graceful Ctrl+C shutdown while generating", "pass": None,
                                   "detail": "NOT RUN: no console to deliver CTRL_C_EVENT in this session; the in-process "
                                             "equivalent is Soak.ShutdownWhileGenerating"})
                signal.signal(signal.SIGINT, prev)
                try:
                    s.close()
                except Exception:
                    pass
        finally:
            srv2.stop()
    finally:
        srv.stop()

    failed = [c["check"] for c in CHECKS if c["pass"] is False]
    out = {"schema_version": 1, "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
           "rounds": a.rounds, "checks": CHECKS, "server_samples": SAMPLES,
           "verdict": "FAIL" if failed else "PASS", "failed": failed}
    with open(os.path.join(a.out, "http-storm.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    print("verdict:", out["verdict"], "failed:", failed)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
