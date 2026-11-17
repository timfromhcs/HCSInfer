#!/usr/bin/env python3
"""Real-model end-to-end run for LFM3-Omni-2B Q8_0 through `HCSInfer serve` (no mocks).

Phases: greedy chat, SSE streaming (TTFT + inter-token rate), client-disconnect cancellation, tool calling
(non-stream and stream) and finally the held-out agentic eval (score.py) against the same server.
Exit codes: 0 PASS, 1 FAIL, 77 BLOCKED (missing prerequisite).

  python -I e2e_lfm3.py --exe <HCSInfer.exe> --model <LFM3-Omni-2B-Q8_0.gguf> --evidence <dir> [--skip-eval]

Stdlib only. Writes <evidence>/e2e-lfm3-<tag>.json with every measured value (one sample per timing: these are
smoke measurements, not the controlled benchmark - use `HCSInfer benchmark` for that).
"""
import argparse
import http.client
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS, FAILS = [], []


def check(name, cond, detail=""):
    RESULTS.append({"check": name, "pass": bool(cond), "detail": str(detail)[:400]})
    print(("PASS  " if cond else "FAIL  ") + name + (("  -- " + str(detail)[:240]) if detail and not cond else ""))
    if not cond:
        FAILS.append(name)


def start_reader(stream, q):
    def run():
        for line in iter(stream.readline, ""):
            q.put(line.rstrip("\r\n"))
        q.put(None)
    threading.Thread(target=run, daemon=True).start()


class Http:
    def __init__(self, port, token):
        self.port, self.token = port, token

    def conn(self, timeout=600):
        return http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)

    def hdr(self):
        return {"Host": "127.0.0.1:%d" % self.port, "Authorization": "Bearer " + self.token,
                "Content-Type": "application/json"}

    def req(self, method, path, body=None, timeout=600):
        c = self.conn(timeout)
        try:
            c.request(method, path, body=json.dumps(body).encode() if body is not None else None, headers=self.hdr())
            r = c.getresponse()
            txt = r.read().decode("utf-8", "replace")
        finally:
            c.close()
        try:
            return r.status, json.loads(txt)
        except ValueError:
            return r.status, txt

    def stream(self, body, timeout=600):
        """POST stream=true; returns (status, deltas list, ttft_s, total_s)."""
        c = self.conn(timeout)
        t0 = time.time()
        c.request("POST", "/v1/chat/completions", body=json.dumps(dict(body, stream=True)).encode(), headers=self.hdr())
        r = c.getresponse()
        deltas, ttft, buf = [], None, b""
        while True:
            chunk = r.read1(4096) if hasattr(r, "read1") else r.read(1)
            if not chunk:
                break
            buf += chunk
            while b"\n\n" in buf:
                ev, buf = buf.split(b"\n\n", 1)
                s = ev.decode("utf-8", "replace")
                if s.startswith("data: ") and "[DONE]" not in s:
                    d = json.loads(s[6:])
                    deltas.append(d)
                    ch = d.get("choices") or [{}]
                    if ttft is None and (ch[0].get("delta") or {}).get("content"):
                        ttft = time.time() - t0
        c.close()
        return r.status, deltas, ttft, time.time() - t0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--model", default=os.environ.get("HCS_TEST_LFM3", ""))
    ap.add_argument("--evidence", default="")
    ap.add_argument("--tag", default="run")
    ap.add_argument("--skip-eval", action="store_true")
    a = ap.parse_args()
    a.exe = os.path.abspath(a.exe)
    if not os.path.isfile(a.exe) or not a.model or not os.path.isfile(a.model):
        print("BLOCKED (NOT RUN): --exe / --model (or HCS_TEST_LFM3) missing")
        return 77
    mdir = os.path.dirname(os.path.abspath(a.model))
    state = tempfile.mkdtemp(prefix="hcs-e2e-lfm3-state-")
    ev = {"schema_version": 1, "tag": a.tag, "model_file": os.path.basename(a.model),
          "model_bytes": os.path.getsize(a.model),
          "exe_version": subprocess.run([a.exe, "--version"], capture_output=True, text=True).stdout.strip()}
    proc = None
    try:
        subprocess.run([a.exe, "--state-dir", state, "models", "scan", mdir, "--json"], capture_output=True, text=True)
        lst = subprocess.run([a.exe, "--state-dir", state, "models", "list", "--json"], capture_output=True, text=True)
        models = json.loads(lst.stdout)
        models = models.get("models", models) if isinstance(models, dict) else models
        stem = os.path.splitext(os.path.basename(a.model))[0].lower()
        mid = next((m["id"] for m in models if m["id"] == stem or (stem in m["id"] and m.get("architecture") != "clip")), None)
        check("scan registers the model", mid, lst.stdout[:300])
        if not mid:
            return 1
        cmd = [a.exe, "--state-dir", state, "serve", "--port", "0", "--model-root", mdir, "--max-body-bytes", "4194304"]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
        q = queue.Queue()
        start_reader(proc.stderr, q)
        port = token = None
        t0 = time.time()
        while time.time() - t0 < 60 and (port is None or token is None):
            try:
                line = q.get(timeout=1)
            except queue.Empty:
                continue
            if line is None:
                break
            if "listening on http://" in line:
                port = int(line.rsplit(":", 1)[1])
            if line.startswith("HCSInfer API token: "):
                token = line.split(": ", 1)[1].strip()
        check("serve prints port and token", port and token)
        if not (port and token):
            return 1
        c = Http(port, token)

        # ---- load (timed) ----
        t = time.time()
        code, j = c.req("POST", "/hcs/v1/models/%s/load" % mid, {})
        check("load request accepted", code in (200, 202), (code, j))
        job = (j or {}).get("job_id") or (j or {}).get("id") if isinstance(j, dict) else None
        for _ in range(600):
            if not job:
                break
            _, st = c.req("GET", "/hcs/v1/jobs/%s" % job)
            if isinstance(st, dict) and st.get("state") in ("succeeded", "failed", "cancelled"):
                check("model load job succeeded", st.get("state") == "succeeded", st)
                break
            time.sleep(1)
        ev["load_seconds"] = round(time.time() - t, 2)
        code, rt = c.req("GET", "/hcs/v1/runtime")
        m_ = rt.get("text_engine", {}).get("metrics", {}) if code == 200 and isinstance(rt, dict) else {}
        ev["backend"], ev["device"] = m_.get("backend"), m_.get("device")
        ev["runtime_metrics"] = m_
        check("runtime reports a backend", bool(m_.get("backend")), rt)

        # ---- greedy chat ----
        body = {"model": mid, "temperature": 0, "max_tokens": 48,
                "messages": [{"role": "user", "content": "Reply with exactly: HCSInfer smoke test passed."}]}
        t = time.time()
        code, j = c.req("POST", "/v1/chat/completions", body)
        ev["chat_first_request_s"] = round(time.time() - t, 3)
        text = j["choices"][0]["message"]["content"] if code == 200 and isinstance(j, dict) else ""
        check("greedy chat -> 200 and answer", code == 200 and text.strip(), (code, j))
        check("answer contains the requested phrase", "smoke test passed" in text.lower(), text)
        ev["chat_answer"] = text
        t = time.time()
        code, j2 = c.req("POST", "/v1/chat/completions", body)
        ev["chat_warm_request_s"] = round(time.time() - t, 3)
        check("greedy decoding is deterministic", code == 200 and j2["choices"][0]["message"]["content"] == text)

        # ---- streaming ----
        sbody = {"model": mid, "temperature": 0, "max_tokens": 160,
                 "messages": [{"role": "user", "content": "Count from 1 to 40, separated by spaces."}]}
        code, deltas, ttft, total = c.stream(sbody)
        streamed = "".join((d["choices"][0].get("delta") or {}).get("content") or "" for d in deltas if d.get("choices"))
        usage = next((d["usage"] for d in reversed(deltas) if d.get("usage")), {})
        check("SSE stream 200 with text", code == 200 and streamed.strip(), (code, streamed[:80]))
        check("stream contains the counting", "10" in streamed and "20" in streamed, streamed[:120])
        ev["stream"] = {"ttft_s": round(ttft, 3) if ttft else None, "total_s": round(total, 3), "chars": len(streamed),
                        "completion_tokens": usage.get("completion_tokens")}
        if usage.get("completion_tokens") and ttft is not None and total > ttft:
            ev["stream"]["decode_tokens_per_s_incl_ttft_excluded"] = round(
                (usage["completion_tokens"] - 1) / (total - ttft), 2)

        # ---- cancellation: disconnect mid-stream, the engine must come back ----
        cc = c.conn()
        long_body = {"model": mid, "temperature": 0, "max_tokens": 1500, "stream": True,
                     "messages": [{"role": "user", "content": "Write a very long story about a lighthouse."}]}
        cc.request("POST", "/v1/chat/completions", body=json.dumps(long_body).encode(), headers=c.hdr())
        r = cc.getresponse()
        got = r.read1(2048) if hasattr(r, "read1") else r.read(64)
        check("long stream started", r.status == 200 and len(got) > 0, r.status)
        t = time.time()
        cc.close()
        ok = False
        while time.time() - t < 30:
            _, rt = c.req("GET", "/hcs/v1/runtime")
            pend = rt.get("jobs", {}) if isinstance(rt, dict) else {}
            code, j3 = c.req("POST", "/v1/chat/completions", dict(body, max_tokens=8))
            if code == 200:
                ok = True
                break
            time.sleep(0.5)
        ev["cancel_recovery_s"] = round(time.time() - t, 2)
        check("client disconnect cancels the work and the engine serves again", ok and ev["cancel_recovery_s"] < 30,
              ev["cancel_recovery_s"])

        # ---- tool calling ----
        tool = {"type": "function", "function": {"name": "get_weather", "description": "Get the current weather for a city.",
                                                 "parameters": {"type": "object", "properties": {"city": {"type": "string"}}, "required": ["city"]}}}
        tb = {"model": mid, "temperature": 0, "max_tokens": 200, "tools": [tool],
              "messages": [{"role": "user", "content": "What's the weather in Paris right now?"}]}
        code, j = c.req("POST", "/v1/chat/completions", tb)
        msg = j["choices"][0]["message"] if code == 200 and isinstance(j, dict) else {}
        calls = msg.get("tool_calls") or []
        check("tools: non-stream returns tool_calls", code == 200 and len(calls) == 1, (code, j))
        if calls:
            args = json.loads(calls[0]["function"]["arguments"])
            check("tools: get_weather(city=Paris)", calls[0]["function"]["name"] == "get_weather" and str(args.get("city", "")).lower() == "paris", calls[0])
            check("tools: finish_reason is tool_calls", j["choices"][0]["finish_reason"] == "tool_calls")
            ev["tool_call_example"] = calls[0]
            # complete the loop with the tool result
            follow = {"model": mid, "temperature": 0, "max_tokens": 120, "tools": [tool],
                      "messages": tb["messages"] + [{"role": "assistant", "content": None, "tool_calls": calls},
                                                    {"role": "tool", "tool_call_id": calls[0]["id"], "content": "{\"temp_c\": 18, \"condition\": \"cloudy\"}"}]}
            code, j4 = c.req("POST", "/v1/chat/completions", follow)
            ans = j4["choices"][0]["message"]["content"] if code == 200 and isinstance(j4, dict) else ""
            check("tools: answer after tool result uses it", code == 200 and "18" in (ans or ""), ans)
            ev["tool_followup_answer"] = ans
        code, deltas, _, _ = c.stream(tb)
        tcs = [d["choices"][0]["delta"]["tool_calls"] for d in deltas if d.get("choices") and (d["choices"][0].get("delta") or {}).get("tool_calls")]
        fin = [d["choices"][0]["finish_reason"] for d in deltas if d.get("choices") and d["choices"][0].get("finish_reason")]
        leaked = "".join((d["choices"][0].get("delta") or {}).get("content") or "" for d in deltas if d.get("choices"))
        check("tools: stream carries tool_calls and finish_reason", code == 200 and tcs and fin[-1:] == ["tool_calls"], (code, fin))
        check("tools: no tag text leaks into streamed content", "<tool_call" not in leaked and "<function=" not in leaked, leaked[:100])

        # ---- held-out agentic eval ----
        if not a.skip_eval:
            out = os.path.join(a.evidence or tempfile.gettempdir(), "agentic-eval-%s.json" % a.tag)
            resp = os.path.join(a.evidence or tempfile.gettempdir(), "agentic-responses-%s.jsonl" % a.tag)
            r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "score.py"), "--base-url", "http://127.0.0.1:%d" % port,
                                "--token", token, "--model", mid, "--out", out, "--save-responses", resp],
                               capture_output=True, text=True)
            print(r.stdout[-1800:])
            check("agentic eval ran", os.path.isfile(out), r.stderr[-300:])
            if os.path.isfile(out):
                ev["agentic_eval"] = json.load(open(out))["summary"]

        ev["checks"] = RESULTS
        ev["result_status"] = "PASS" if not FAILS else "FAIL"
        if a.evidence:
            os.makedirs(a.evidence, exist_ok=True)
            with open(os.path.join(a.evidence, "e2e-lfm3-%s.json" % a.tag), "w", encoding="utf-8") as f:
                json.dump(ev, f, indent=2)
        print("\nSUMMARY: %d checks, %d failed" % (len(RESULTS), len(FAILS)))
        for f in FAILS:
            print("  FAILED:", f)
        return 0 if not FAILS else 1
    finally:
        if proc:
            proc.kill()
        shutil.rmtree(state, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
