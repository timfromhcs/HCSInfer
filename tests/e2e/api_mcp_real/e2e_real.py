"""Real-engine end-to-end test for `HCSInfer serve` (HTTP, via curl.exe) and `HCSInfer mcp stdio`
(scripted JSON-RPC client). No mocks: requires a real GGUF model (HCS_TEST_GGUF) and a binary built
with the llama engine. Exit codes: 0 PASS, 1 FAIL, 77 BLOCKED (missing prerequisite; ctest SKIP).

Run:  python -I e2e_real.py --exe <HCSInfer.exe> [--model <gguf>] [--evidence <dir>]
Stdlib only. Evidence (JSON) is written to --evidence.
"""
import argparse
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time

RESULTS = []
FAILS = []


def check(name, cond, detail=""):
    RESULTS.append({"check": name, "pass": bool(cond), "detail": str(detail)[:400]})
    print(("PASS  " if cond else "FAIL  ") + name + (("  -- " + str(detail)[:200]) if detail and not cond else ""))
    if not cond:
        FAILS.append(name)


def start_reader(stream, q):
    def run():
        for line in iter(stream.readline, ""):
            q.put(line.rstrip("\r\n"))
        q.put(None)
    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t


class Curl:
    def __init__(self, base, token):
        self.base, self.token = base, token
        self.tmp = tempfile.mkdtemp(prefix="hcs-e2e-curl-")

    def req(self, method, path, body=None, token=True, headers=None, timeout=300, raw_body=None):
        hdr = os.path.join(self.tmp, "h.txt")
        out = os.path.join(self.tmp, "b.txt")
        cmd = ["curl.exe", "-s", "-S", "-m", str(timeout), "-X", method, "-D", hdr, "-o", out, "-w", "%{http_code}"]
        if token:
            cmd += ["-H", "Authorization: Bearer " + self.token]
        for k, v in (headers or {}).items():
            cmd += ["-H", f"{k}: {v}"]
        data = raw_body if raw_body is not None else (json.dumps(body) if body is not None else None)
        if data is not None:
            dfile = os.path.join(self.tmp, "d.json")
            with open(dfile, "w", encoding="utf-8") as f:
                f.write(data)
            cmd += ["-H", "Content-Type: application/json", "--data-binary", "@" + dfile]
        cmd.append(self.base + path)
        p = subprocess.run(cmd, capture_output=True, text=True)
        code = p.stdout.strip()
        text = open(out, encoding="utf-8", errors="replace").read() if os.path.exists(out) else ""
        return (int(code) if code.isdigit() else 0), text, p.stderr.strip()

    def jreq(self, *a, **k):
        code, text, err = self.req(*a, **k)
        try:
            return code, json.loads(text) if text else None
        except ValueError:
            return code, {"_raw": text[:300], "_err": err}


def find_model_id(models, filename_stem):
    for m in models:  # exact id first; a sibling mmproj contains the model's stem too and is never the main model
        if m["id"] == filename_stem:
            return m["id"]
    for m in models:
        if filename_stem in m["id"] and not m["id"].startswith("mmproj"):
            return m["id"]
    return None


def http_phase(exe, model_dir, model_stem, state_dir, evidence):
    print("== HTTP phase (serve + curl.exe) ==")
    proc = subprocess.Popen([exe, "--state-dir", state_dir, "serve", "--port", "0", "--model-root", model_dir],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
    q = queue.Queue()
    start_reader(proc.stderr, q)
    port = token = None
    t0 = time.time()
    stderr_lines = []
    while time.time() - t0 < 30 and (port is None or token is None):
        try:
            line = q.get(timeout=1)
        except queue.Empty:
            continue
        if line is None:
            break
        stderr_lines.append(line)
        if "listening on http://" in line:
            port = int(line.rsplit(":", 1)[1])
        if line.startswith("HCSInfer API token: "):
            token = line.split(": ", 1)[1].strip()
    check("serve prints port and random token on stderr", port and token and len(token) >= 32, stderr_lines[-3:])
    if not (port and token):
        proc.kill()
        return {}
    try:
        base = f"http://127.0.0.1:{port}"
        c = Curl(base, token)
        ev = {"port_bound": port, "token_len": len(token)}

        code, j = c.jreq("GET", "/hcs/v1/health", token=False)
        check("health without token", code == 200 and j and j.get("status") == "ok", (code, j))
        code, j = c.jreq("GET", "/v1/models", token=False)
        check("auth: missing token -> 401", code == 401, code)
        code, j = c.jreq("GET", "/v1/models", headers={"Authorization": "Bearer wrong"}, token=False)
        check("auth: wrong token -> 401", code == 401, code)
        code, j = c.jreq("GET", "/v1/models", headers={"Host": "evil.example.com"})
        check("DNS-rebinding Host -> 403", code == 403, (code, j))
        code, j = c.jreq("GET", "/v1/models", headers={"Origin": "http://evil.example"})
        check("foreign Origin -> 403", code == 403, (code, j))
        code, j = c.jreq("POST", "/v1/chat/completions", raw_body="{not json")
        check("malformed JSON -> 400", code == 400, (code, j))
        code, j = c.jreq("POST", "/v1/chat/completions", raw_body='{"messages":[{"role":"user","content":"' + "x" * (2 * 1024 * 1024) + '"}]}')
        check("oversize body -> 413", code == 413, (code, j))
        code, j = c.jreq("POST", "/hcs/v1/models/..%2f..%2fwindows/load")
        check("path traversal in model id rejected (400/404)", code in (400, 404), (code, j))
        code, j = c.jreq("POST", "/v1/images/generations", {"prompt": "x"})
        # The image route is no longer a stub. A build without a diffusion engine must answer 501/unsupported; a
        # build with one must not render without a model: it answers 409/invalid_state (this test registers only the
        # text model). A 200 without a model, a 404 or any other code would be a defect. The full render path is
        # covered by e2e_image_real.
        check("images endpoint is real, not a stub (501 without engine / 409 without image model)",
              (code == 501 and j and j["error"]["code"] == "unsupported") or
              (code == 409 and j and j["error"]["code"] == "invalid_state"), (code, j))

        code, j = c.jreq("GET", "/v1/models")
        check("GET /v1/models", code == 200 and j and j["object"] == "list", (code, j))
        mid = find_model_id(j["data"], model_stem) if j else None
        check("model is registered", mid is not None, [m["id"] for m in (j or {}).get("data", [])][:5])
        if not mid:
            return ev
        for p in ("runtime", "devices", "memory", "models"):
            code, jj = c.jreq("GET", "/hcs/v1/" + p)
            check(f"GET /hcs/v1/{p}", code == 200, code)
            ev["hcs_" + p] = jj

        # Load as a job
        t = time.time()
        code, j = c.jreq("POST", f"/hcs/v1/models/{mid}/load")
        check("model load returns 202 + job id", code == 202 and j and "job_id" in j, (code, j))
        job = j["job_id"]
        state = None
        for _ in range(600):
            code, js = c.jreq("GET", f"/hcs/v1/jobs/{job}")
            state = js.get("state") if js else None
            if state in ("succeeded", "failed", "cancelled"):
                break
            time.sleep(0.5)
        ev["load_wall_s"] = round(time.time() - t, 2)
        check("model load job succeeded", state == "succeeded", js)
        code, mj = c.jreq("GET", f"/hcs/v1/models/{mid}")
        check("model state loaded", mj and mj.get("state") == "loaded", mj)
        code, rt = c.jreq("GET", "/hcs/v1/runtime")
        ev["runtime_after_load"] = rt

        # Real chat, non-stream
        body = {"model": mid, "messages": [{"role": "user", "content": "Reply with exactly: HCSInfer API test"}],
                "max_tokens": 48, "temperature": 0, "seed": 1}
        t = time.time()
        code, j = c.jreq("POST", "/v1/chat/completions", body)
        ev["chat_nonstream_wall_s"] = round(time.time() - t, 2)
        text = j["choices"][0]["message"]["content"] if code == 200 and j else ""
        check("real chat completion (non-stream) returns text", code == 200 and len(text.strip()) > 0, (code, j))
        ev["chat_nonstream"] = {"status": code, "text_preview": text[:120], "usage": (j or {}).get("usage"),
                                "finish_reason": (j or {}).get("choices", [{}])[0].get("finish_reason")}
        check("usage reports completion tokens", bool(j and j.get("usage") and j["usage"]["completion_tokens"] > 0), (j or {}).get("usage"))

        # Real chat, SSE via curl -N
        body["stream"] = True
        cp = subprocess.Popen(["curl.exe", "-s", "-N", "-m", "300", "-H", "Authorization: Bearer " + token,
                               "-H", "Content-Type: application/json", "--data-binary", json.dumps(body),
                               base + "/v1/chat/completions"], stdout=subprocess.PIPE, text=True, encoding="utf-8")
        events, stamps = [], []
        for line in cp.stdout:
            line = line.strip()
            if line.startswith("data: "):
                events.append(line[6:])
                stamps.append(time.time())
        cp.wait()
        content = ""
        for e in events:
            if e != "[DONE]":
                d = json.loads(e)["choices"][0]["delta"]
                content += d.get("content", "") or ""
        check("SSE: multiple chunks and [DONE]", len(events) >= 4 and events[-1] == "[DONE]", len(events))
        check("SSE: streamed text non-empty", len(content.strip()) > 0, content[:80])
        check("SSE: delivered incrementally", len(stamps) >= 4 and stamps[-1] - stamps[1] > 0.05, (stamps[-1] - stamps[1]) if len(stamps) > 1 else 0)
        ev["chat_stream"] = {"events": len(events), "text_preview": content[:120]}

        # Cancel a long real generation
        longb = {"model": mid, "messages": [{"role": "user", "content": "Write a very long story about the sea."}],
                 "max_tokens": 4000, "temperature": 0.8, "stream": True}
        cp = subprocess.Popen(["curl.exe", "-s", "-N", "-m", "300", "-H", "Authorization: Bearer " + token,
                               "-H", "Content-Type: application/json", "--data-binary", json.dumps(longb),
                               base + "/v1/chat/completions"], stdout=subprocess.PIPE, text=True, encoding="utf-8")
        jobid, n_chunks = None, 0
        t = time.time()
        for line in cp.stdout:
            line = line.strip()
            if not line.startswith("data: ") or line == "data: [DONE]":
                continue
            d = json.loads(line[6:])
            if "choices" in d:
                n_chunks += 1
                if jobid is None:
                    jobid = "job-" + d["id"].split("-")[-1]
            if n_chunks >= 5:
                break
        check("long generation started streaming", jobid is not None and n_chunks >= 5, (jobid, n_chunks))
        cc, cj = c.jreq("POST", f"/hcs/v1/jobs/{jobid}/cancel")
        check("cancel endpoint accepted", cc == 200, (cc, cj))
        rest = cp.stdout.read()
        cp.wait()
        final = None
        for _ in range(100):
            cc, js = c.jreq("GET", f"/hcs/v1/jobs/{jobid}")
            final = js.get("state") if js else None
            if final in ("cancelled", "failed", "succeeded"):
                break
            time.sleep(0.1)
        check("cancelled job ends in state cancelled", final == "cancelled", js)
        ev["cancel"] = {"job": jobid, "final_state": final, "seconds_to_cancel": round(time.time() - t, 2)}
        check("stream terminated after cancel (curl exited)", cp.returncode is not None, cp.returncode)

        # engine still healthy after cancel
        body["stream"] = False
        code, j = c.jreq("POST", "/v1/chat/completions", body)
        check("chat works after a cancel", code == 200 and j and j["choices"][0]["message"]["content"].strip() != "", code)

        # Client DISCONNECT mid-stream (not the cancel endpoint): the server must notice the dead socket, stop the
        # real generation and free the job.
        longb2 = dict(longb)
        cp = subprocess.Popen(["curl.exe", "-s", "-N", "-m", "300", "-H", "Authorization: Bearer " + token,
                               "-H", "Content-Type: application/json", "--data-binary", json.dumps(longb2),
                               base + "/v1/chat/completions"], stdout=subprocess.PIPE, text=True, encoding="utf-8")
        jobid2, n_chunks = None, 0
        for line in cp.stdout:
            line = line.strip()
            if not line.startswith("data: ") or line == "data: [DONE]":
                continue
            d = json.loads(line[6:])
            if "choices" in d:
                n_chunks += 1
                if jobid2 is None:
                    jobid2 = "job-" + d["id"].split("-")[-1]
            if n_chunks >= 5:
                break
        cp.kill()  # the client vanishes without telling anybody
        cp.wait()
        t = time.time()
        final2 = None
        for _ in range(300):
            cc, js = c.jreq("GET", f"/hcs/v1/jobs/{jobid2}")
            final2 = js.get("state") if js else None
            if final2 in ("cancelled", "failed", "succeeded"):
                break
            time.sleep(0.1)
        ev["disconnect"] = {"job": jobid2, "final_state": final2, "seconds_to_stop": round(time.time() - t, 2)}
        check("client disconnect mid-stream stops the real generation (job cancelled)", final2 == "cancelled", js)
        check("disconnect freed the job within 30 s", time.time() - t < 30, time.time() - t)
        code, rt = c.jreq("GET", "/hcs/v1/runtime")
        check("no job left pending after the disconnect", rt and rt.get("jobs_pending") == 0, rt and rt.get("jobs_pending"))
        body["stream"] = False
        code, j = c.jreq("POST", "/v1/chat/completions", body)
        check("chat works after a client disconnect", code == 200 and j and j["choices"][0]["message"]["content"].strip() != "", code)

        # CLI client against the running server (real exe, real model; token via the environment)
        cenv = dict(os.environ, HCS_API_TOKEN=token)
        cli = [exe, "--state-dir", state_dir, "models"]
        conn = ["--port", str(port)]

        def run_cli(*a):
            r = subprocess.run(cli + list(a) + conn, capture_output=True, text=True, env=cenv, timeout=600)
            return r.returncode, (r.stdout + r.stderr).strip()

        rc, txt = run_cli("status", "--json")
        check("CLI models status sees the loaded model", rc == 0 and json.loads(txt).get("loaded_model") == mid, (rc, txt[:200]))
        rc, txt = run_cli("unload")
        check("CLI models unload (no id) unloads the loaded model", rc == 0 and ("unloaded " + mid) in txt, (rc, txt))
        rc, txt = run_cli("unload")
        check("CLI models unload with nothing loaded -> exit 11", rc == 11, (rc, txt))
        t = time.time()
        rc, txt = run_cli("load", mid)
        ev["cli_load_wall_s"] = round(time.time() - t, 2)
        check("CLI models load <id> waits for the real load", rc == 0 and ("loaded " + mid) in txt, (rc, txt))
        code, j = c.jreq("POST", "/v1/chat/completions", body)
        check("chat works after a CLI-driven load", code == 200 and j and j["choices"][0]["message"]["content"].strip() != "", code)
        rc, txt = run_cli("load", "no-such-model")
        check("CLI models load unknown id -> exit 7", rc == 7, (rc, txt))

        code, j = c.jreq("POST", f"/hcs/v1/models/{mid}/unload")
        check("unload", code == 200, (code, j))
        return ev
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


class McpClient:
    def __init__(self, exe, state_dir, model_dir):
        self.p = subprocess.Popen([exe, "--state-dir", state_dir, "mcp", "stdio", "--model-root", model_dir],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, encoding="utf-8", bufsize=1)
        self.out = queue.Queue()
        self.raw_lines = []
        self.err = []
        start_reader(self.p.stdout, self.out)
        eq = queue.Queue()
        start_reader(self.p.stderr, eq)
        self.errq = eq
        self.nid = 0

    def send(self, obj):
        self.p.stdin.write(json.dumps(obj) + "\n")
        self.p.stdin.flush()

    def recv(self, timeout=300):
        line = self.out.get(timeout=timeout)
        if line is not None:
            self.raw_lines.append(line)
        return line

    def request(self, method, params=None, modern=True, timeout=300):
        self.nid += 1
        params = dict(params or {})
        if modern:
            params["_meta"] = {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
                               "io.modelcontextprotocol/clientInfo": {"name": "e2e", "version": "1"},
                               "io.modelcontextprotocol/clientCapabilities": {}}
        rid = self.nid
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        while True:
            line = self.recv(timeout)
            if line is None:
                return None
            msg = json.loads(line)
            if msg.get("id") == rid:
                return msg

    def tool(self, name, args=None, timeout=300):
        return self.request("tools/call", {"name": name, "arguments": args or {}}, timeout=timeout)


def vault_phase(exe, model_dir, model_stem, state_dir):
    """Opt-in Model Vault stage with the real engine: `models hash` records the digest, `serve --vault-prefetch`
    reads the file through the bounded reader, verifies it against that digest, then the engine loads and
    answers a real chat. Also proves that no file appeared next to the model."""
    print("== Vault phase (serve --vault-prefetch) ==")
    ev = {}
    before = sorted(os.listdir(model_dir))
    r = subprocess.run([exe, "--state-dir", state_dir, "models", "list", "--json"], capture_output=True, text=True)
    mid = find_model_id(json.loads(r.stdout)["models"], model_stem)
    msize = next((m["size_bytes"] for m in json.loads(r.stdout)["models"] if m["id"] == mid), 0)
    check("vault: model is registered", mid is not None, r.stdout[:200])
    if not mid:
        return ev
    r = subprocess.run([exe, "--state-dir", state_dir, "models", "hash", mid, "--json"], capture_output=True, text=True)
    check("vault: models hash records the digest", r.returncode == 0, (r.returncode, r.stderr[:200]))
    digest = json.loads(r.stdout)["sha256"] if r.returncode == 0 else ""
    ev["hash"] = digest
    proc = subprocess.Popen([exe, "--state-dir", state_dir, "serve", "--port", "0", "--model-root", model_dir,
                             "--vault-prefetch", "--vault-buffers", "3", "--vault-chunk-mib", "8"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
    q = queue.Queue()
    start_reader(proc.stderr, q)
    port = token = None
    t0 = time.time()
    while time.time() - t0 < 30 and (port is None or token is None):
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
    if not (port and token):
        check("vault: serve started", False, "no port/token")
        proc.kill()
        return ev
    try:
        c = Curl(f"http://127.0.0.1:{port}", token)
        code, j = c.jreq("POST", f"/hcs/v1/models/{mid}/load")
        job = j["job_id"]
        state = None
        for _ in range(1200):
            code, js = c.jreq("GET", f"/hcs/v1/jobs/{job}")
            state = js.get("state") if js else None
            if state in ("succeeded", "failed", "cancelled"):
                break
            time.sleep(0.5)
        check("vault: load through the vault stage succeeded", state == "succeeded", js)
        code, rt = c.jreq("GET", "/hcs/v1/runtime")
        v = (rt or {}).get("vault", {}).get("model", {})
        ev["report"] = v
        check("vault: runtime reports the stage", rt and rt.get("vault_prefetch") is True and v.get("bytes") == msize, v)
        check("vault: digest verified against the manifest", v.get("verified_against_manifest") is True and v.get("sha256") == digest, v)
        check("vault: buffer RAM bounded (<= 3 slots x 8 MiB x 2)", 0 < v.get("buffer_bytes", 0) <= 3 * 8 * 2 * (1 << 20), v.get("buffer_bytes"))
        body = {"model": mid, "messages": [{"role": "user", "content": "Reply with exactly: vault test"}],
                "max_tokens": 32, "temperature": 0, "seed": 1}
        code, j = c.jreq("POST", "/v1/chat/completions", body)
        check("vault: real chat after a vault load", code == 200 and j and j["choices"][0]["message"]["content"].strip() != "", code)
        c.jreq("POST", f"/hcs/v1/models/{mid}/unload")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    check("vault: nothing was created next to the model", sorted(os.listdir(model_dir)) == before, os.listdir(model_dir))
    return ev


def mcp_phase(exe, model_dir, model_stem, state_dir):
    print("== MCP phase (mcp stdio, scripted client) ==")
    m = McpClient(exe, state_dir, model_dir)
    ev = {}
    try:
        r = m.request("server/discover")
        check("MCP server/discover", r and "result" in r and "2026-07-28" in r["result"]["supportedVersions"], r)
        r = m.request("tools/list")
        names = [t["name"] for t in r["result"]["tools"]]
        check("MCP tools/list has required tools",
              all(n in names for n in ("hcs.models.list", "hcs.models.load", "hcs.chat.generate", "hcs.runtime.status", "hcs.jobs.cancel")), names)
        r = m.tool("hcs.models.list")
        models = r["result"]["structuredContent"]["models"]
        mid = find_model_id(models, model_stem)
        check("MCP models.list finds the model", mid is not None, [x["id"] for x in models][:5])
        t = time.time()
        r = m.tool("hcs.models.load", {"id": mid})
        ev["load_wall_s"] = round(time.time() - t, 2)
        check("MCP models.load (real)", r["result"]["isError"] is False, r)
        t = time.time()
        r = m.tool("hcs.chat.generate", {"prompt": "Reply with exactly: HCSInfer MCP test", "max_tokens": 48, "temperature": 0, "seed": 1})
        ev["chat_wall_s"] = round(time.time() - t, 2)
        sc = r["result"].get("structuredContent", {})
        check("MCP chat.generate (real) returns text", r["result"]["isError"] is False and len(sc.get("text", "").strip()) > 0, r)
        ev["chat"] = {"text_preview": sc.get("text", "")[:120], "usage": sc.get("usage"), "finish_reason": sc.get("finish_reason")}
        r = m.tool("hcs.runtime.status", {"include_devices": True})
        check("MCP runtime.status", r["result"]["isError"] is False, r)
        ev["runtime_status"] = r["result"].get("structuredContent")
        r = m.tool("hcs.models.load", {"id": "../../windows/system32"})
        check("MCP path traversal id rejected", r["result"]["isError"] is True, r)
        r = m.tool("hcs.vision.analyze", {"prompt": "x", "image_path": "C:\\Windows\\win.ini"})
        check("MCP image path outside approved roots -> security_violation",
              r["result"]["structuredContent"]["error"]["code"] == "security_violation", r)

        # cancellation over MCP of a long real generation
        m.nid += 1
        rid = m.nid
        params = {"name": "hcs.chat.generate", "arguments": {"prompt": "Write a very long story about the sea.", "max_tokens": 4000},
                  "_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28", "io.modelcontextprotocol/clientCapabilities": {}}}
        m.send({"jsonrpc": "2.0", "id": rid, "method": "tools/call", "params": params})
        time.sleep(2.0)
        t = time.time()
        m.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": rid, "reason": "e2e"}})
        # The cancelled request must not be answered; a following request is answered promptly.
        r = m.request("tools/list", timeout=60)
        ev["cancel_followup_s"] = round(time.time() - t, 2)
        check("MCP: server responsive after cancellation", r is not None and "result" in r, r)
        time.sleep(0.5)
        answered = []
        try:
            while True:
                line = m.out.get_nowait()
                if line:
                    m.raw_lines.append(line)
                    answered.append(json.loads(line).get("id"))
        except queue.Empty:
            pass
        check("MCP: no response sent for the cancelled request", rid not in answered and all(json.loads(l).get("id") != rid for l in m.raw_lines), answered)
        r = m.tool("hcs.chat.generate", {"prompt": "Say hi", "max_tokens": 16})
        check("MCP: chat works after cancel", r["result"]["isError"] is False, r)

        # legacy era handshake (separate process)
        m.p.stdin.close()
        try:
            rc = m.p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            m.p.kill()
            rc = -1
        check("MCP: clean exit on stdin EOF", rc == 0, rc)
        # stdout cleanliness: every line is a JSON-RPC message
        bad = []
        for l in m.raw_lines:
            try:
                j = json.loads(l)
                if j.get("jsonrpc") != "2.0" or not ("result" in j or "error" in j or "method" in j):
                    bad.append(l[:80])
            except ValueError:
                bad.append(l[:80])
        rest = []
        while True:
            try:
                l = m.out.get_nowait()
            except queue.Empty:
                break
            if l:
                rest.append(l)
        for l in rest:
            try:
                json.loads(l)
            except ValueError:
                bad.append(l[:80])
        check("MCP: stdout carries only JSON-RPC (%d lines)" % len(m.raw_lines), not bad, bad[:3])
        stderr_lines = []
        while True:
            try:
                l = m.errq.get_nowait()
            except queue.Empty:
                break
            if l:
                stderr_lines.append(l)
        check("MCP: logs went to stderr", any('"level"' in l for l in stderr_lines), stderr_lines[:2])
        ev["stdout_lines"] = len(m.raw_lines)
    finally:
        if m.p.poll() is None:
            m.p.kill()

    m2 = McpClient(exe, state_dir, model_dir)
    try:
        m2.nid += 1
        m2.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                 "params": {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "old", "version": "1"}}})
        r = json.loads(m2.recv(30))
        check("MCP legacy initialize", r.get("result", {}).get("protocolVersion") == "2025-11-25", r)
        m2.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        m2.send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        r = json.loads(m2.recv(30))
        check("MCP legacy tools/list", len(r.get("result", {}).get("tools", [])) >= 11, r)
    finally:
        m2.p.stdin.close()
        try:
            m2.p.wait(timeout=20)
        except subprocess.TimeoutExpired:
            m2.p.kill()
    return ev


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--model", default=os.environ.get("HCS_TEST_GGUF", ""))
    ap.add_argument("--evidence", default="")
    a = ap.parse_args()
    if not os.path.isfile(a.exe):
        print("BLOCKED: executable not found:", a.exe)
        return 77
    if not a.model or not os.path.isfile(a.model):
        print("BLOCKED: HCS_TEST_GGUF not set / model missing")
        return 77
    if shutil.which("curl.exe") is None and shutil.which("curl") is None:
        print("BLOCKED: curl.exe not found")
        return 77
    model_dir = os.path.dirname(os.path.abspath(a.model))
    stem = os.path.splitext(os.path.basename(a.model))[0].lower()
    state = tempfile.mkdtemp(prefix="hcs-e2e-state-")
    try:
        r = subprocess.run([a.exe, "--state-dir", state, "models", "scan", model_dir], capture_output=True, text=True)
        print(r.stdout.strip().splitlines()[0] if r.stdout.strip() else r.stderr.strip())
        # Build identity evidence
        v = subprocess.run([a.exe, "--version"], capture_output=True, text=True).stdout.strip()
        ev = {"schema_version": 1, "exe": os.path.basename(a.exe), "version": v, "model_file": os.path.basename(a.model),
              "model_size_bytes": os.path.getsize(a.model)}
        ev["http"] = http_phase(a.exe, model_dir, stem, state, a.evidence)
        ev["vault"] = vault_phase(a.exe, model_dir, stem, state)
        ev["mcp"] = mcp_phase(a.exe, model_dir, stem, state)
        ev["checks"] = RESULTS
        ev["result_status"] = "PASS" if not FAILS else "FAIL"
        if a.evidence:
            os.makedirs(a.evidence, exist_ok=True)
            with open(os.path.join(a.evidence, "e2e-real.json"), "w", encoding="utf-8") as f:
                json.dump(ev, f, indent=2)
        print("\nSUMMARY: %d checks, %d failed" % (len(RESULTS), len(FAILS)))
        for f in FAILS:
            print("  FAILED:", f)
        return 0 if not FAILS else 1
    finally:
        shutil.rmtree(state, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
