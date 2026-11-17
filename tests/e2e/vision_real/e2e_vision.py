"""Real-model end-to-end test for image input through `HCSInfer serve` (OpenAI-style image_url data URI on
/v1/chat/completions, plain + SSE streaming + client-disconnect cancellation) and `HCSInfer mcp stdio`
(hcs.vision.analyze). No mocks: needs a real multimodal GGUF + its mmproj (siblings in one folder) and a binary
built with the llama engine. Exit codes: 0 PASS, 1 FAIL, 77 BLOCKED (missing prerequisite; ctest SKIP).

Run:  python -I e2e_vision.py --exe <HCSInfer.exe> --vlm <model.gguf> [--evidence <dir>]
Stdlib only. The test image is generated here (solid red square on white): redistributable.
"""
import argparse
import base64
import http.client
import json
import os
import queue
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

RESULTS, FAILS = [], []


def check(name, cond, detail=""):
    RESULTS.append({"check": name, "pass": bool(cond), "detail": str(detail)[:400]})
    print(("PASS  " if cond else "FAIL  ") + name + (("  -- " + str(detail)[:240]) if detail and not cond else ""))
    if not cond:
        FAILS.append(name)


def png_chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))


def make_png(w, h, pixel):
    raw = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            png_chunk(b"IDAT", zlib.compress(raw)) + png_chunk(b"IEND", b""))


def red_square(size=256):
    return make_png(size, size, lambda x, y: (220, 20, 20) if size // 4 <= x < 3 * size // 4 and size // 4 <= y < 3 * size // 4 else (255, 255, 255))


def data_uri(png):
    return "data:image/png;base64," + base64.b64encode(png).decode()


def start_reader(stream, q):
    def run():
        for line in iter(stream.readline, ""):
            q.put(line.rstrip("\r\n"))
        q.put(None)
    threading.Thread(target=run, daemon=True).start()


class Http:
    def __init__(self, port, token):
        self.port, self.token = port, token

    def req(self, method, path, body=None, token=True, timeout=300, raw=None):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        h = {"Host": f"127.0.0.1:{self.port}"}
        if token:
            h["Authorization"] = "Bearer " + self.token
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        if data is not None:
            h["Content-Type"] = "application/json"
        try:
            c.request(method, path, body=data, headers=h)
            r = c.getresponse()
            txt = r.read().decode("utf-8", "replace")
        except (ConnectionError, OSError) as e:  # server may close on an over-limit body before we finish sending
            return -1, str(e)
        finally:
            c.close()
        try:
            return r.status, json.loads(txt)
        except ValueError:
            return r.status, txt


def chat_body(model, uri, text="What colour is the large square in this image? Answer in one short sentence.", **kw):
    b = {"model": model, "max_tokens": 40, "temperature": 0,
         "messages": [{"role": "user", "content": [{"type": "text", "text": text},
                                                    {"type": "image_url", "image_url": {"url": uri}}]}]}
    b.update(kw)
    return b


def http_phase(exe, state, roots, model_id, evidence):
    print("== HTTP phase (serve, data-URI image input) ==")
    cmd = [exe, "--state-dir", state, "serve", "--port", "0", "--max-body-bytes", "4194304"]
    for r in roots:
        cmd += ["--model-root", r]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
    q = queue.Queue()
    start_reader(proc.stderr, q)
    port = token = None
    t0, lines = time.time(), []
    while time.time() - t0 < 30 and (port is None or token is None):
        try:
            line = q.get(timeout=1)
        except queue.Empty:
            continue
        if line is None:
            break
        lines.append(line)
        if "listening on http://" in line:
            port = int(line.rsplit(":", 1)[1])
        if line.startswith("HCSInfer API token: "):
            token = line.split(": ", 1)[1].strip()
    check("serve prints port and token", port and token, lines[-3:])
    if not (port and token):
        proc.kill()
        return {}
    ev = {}
    try:
        c = Http(port, token)
        png = red_square()
        uri = data_uri(png)

        t = time.time()
        code, j = c.req("POST", "/v1/chat/completions", chat_body(model_id, uri))
        ev["nonstream_ms"] = int((time.time() - t) * 1000)
        text = j["choices"][0]["message"]["content"] if code == 200 and isinstance(j, dict) else ""
        check("image_url data URI -> 200 with an answer", code == 200 and text.strip(), (code, j))
        check("answer mentions the red square", "red" in text.lower(), text)
        usage = (j or {}).get("usage") or {} if isinstance(j, dict) else {}
        check("usage.prompt_tokens shows the image was consumed (>60)", usage.get("prompt_tokens", 0) > 60, usage)
        ev["answer"], ev["usage"] = text, usage
        code, rt = c.req("GET", "/hcs/v1/runtime")
        m_ = (rt.get("text_engine", {}).get("metrics", {}) if code == 200 and isinstance(rt, dict) else {})
        ev["runtime_backend"], ev["runtime_device"] = m_.get("backend"), m_.get("device")
        check("runtime reports the backend that served the image request", bool(m_.get("backend")), rt)

        code, j2 = c.req("POST", "/v1/chat/completions", chat_body(model_id, uri))
        check("greedy answer is deterministic across requests", code == 200 and j2["choices"][0]["message"]["content"] == text, j2)

        # SSE streaming
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=300)
        conn.request("POST", "/v1/chat/completions", body=json.dumps(chat_body(model_id, uri, stream=True)),
                     headers={"Authorization": "Bearer " + token, "Content-Type": "application/json", "Host": f"127.0.0.1:{port}"})
        r = conn.getresponse()
        body = r.read().decode()
        conn.close()
        deltas = [json.loads(l[6:]) for l in body.split("\n\n") if l.startswith("data: {")]
        streamed = "".join(d["choices"][0]["delta"].get("content", "") for d in deltas if d.get("choices"))
        check("SSE stream: 200, text/event-stream, [DONE]", r.status == 200 and "text/event-stream" in (r.getheader("Content-Type") or "") and "[DONE]" in body, (r.status, body[-120:]))
        check("SSE stream text equals non-stream text", streamed == text, (streamed, text))

        # rejected inputs (all 400, none reaches the engine)
        bads = {
            "remote https URL": chat_body(model_id, "https://example.com/cat.png"),
            "file:// URL": chat_body(model_id, "file:///C:/Windows/win.ini"),
            "local path": chat_body(model_id, "C:\\Windows\\win.ini"),
            "bad base64": chat_body(model_id, "data:image/png;base64,@@@@"),
            "non-image media type": chat_body(model_id, "data:text/plain;base64,QUJD"),
            "bytes that are not an image": chat_body(model_id, "data:image/png;base64," + base64.b64encode(b"not an image " * 8).decode()),
            "huge declared dimensions": chat_body(model_id, data_uri(b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 100000, 100000, 8, 2, 0, 0, 0)) + png_chunk(b"IDAT", b"\x00"))),
        }
        for name, body_ in bads.items():
            code, j = c.req("POST", "/v1/chat/completions", body_)
            check(f"rejected: {name} -> 400", code == 400, (code, j))
        two = chat_body(model_id, uri)
        two["messages"][0]["content"].append({"type": "image_url", "image_url": {"url": uri}})
        code, j = c.req("POST", "/v1/chat/completions", two)
        check("rejected: two images -> 400", code == 400, (code, j))
        code, j = c.req("POST", "/v1/chat/completions", None, raw=b'{"model":"x","messages":[{"role":"user","content":"' + b"A" * (5 * 1024 * 1024) + b'"}]}')
        check("rejected: oversize body -> 413", code in (413, -1), (code,))
        code, j = c.req("POST", "/v1/chat/completions", chat_body(model_id, uri), token=False)
        check("image request without token -> 401", code == 401, (code,))

        # client disconnect cancels the vision job
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=300)
        long_body = chat_body(model_id, uri, "Write a very long, detailed story about this image.", stream=True, max_tokens=800)
        conn.request("POST", "/v1/chat/completions", body=json.dumps(long_body),
                     headers={"Authorization": "Bearer " + token, "Content-Type": "application/json", "Host": f"127.0.0.1:{port}"})
        r = conn.getresponse()
        first = r.read(400)
        job = None
        for tok in first.decode("utf-8", "replace").split('"'):
            if tok.startswith("job-"):
                job = tok
        conn.close()  # disconnect mid-stream
        t_disc = time.time()
        ok = False
        if True:
            for _ in range(100):
                code, runtime = c.req("GET", "/hcs/v1/runtime")
                code2, _m = c.req("GET", "/hcs/v1/memory")
                if code == 200 and runtime.get("jobs_pending", 1) == 0:
                    ok = True
                    break
                time.sleep(0.2)
        waited = time.time() - t_disc
        # 800 requested tokens take far longer than this on CPU: finishing within seconds means it was cancelled
        check("client disconnect during streaming cancels the work (no job pending)", ok and waited < 10, (ok, round(waited, 1)))
        ev["cancel_after_disconnect_s"] = round(waited, 2)
        code, j = c.req("POST", "/v1/chat/completions", chat_body(model_id, uri))
        check("engine healthy after cancellation", code == 200 and "red" in j["choices"][0]["message"]["content"].lower(), (code, j))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()
    return ev


class Mcp:
    def __init__(self, exe, state, roots):
        cmd = [exe, "--state-dir", state, "mcp", "stdio"]
        for r in roots:
            cmd += ["--model-root", r]
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)
        self.out = queue.Queue()
        start_reader(self.p.stdout, self.out)
        start_reader(self.p.stderr, queue.Queue())
        self.n = 0

    def tool(self, name, args, timeout=300):
        self.n += 1
        meta = {"io.modelcontextprotocol/protocolVersion": "2026-07-28", "io.modelcontextprotocol/clientInfo": {"name": "e2e", "version": "1"}, "io.modelcontextprotocol/clientCapabilities": {}}
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.n, "method": "tools/call", "params": {"name": name, "arguments": args, "_meta": meta}}) + "\n")
        self.p.stdin.flush()
        while True:
            line = self.out.get(timeout=timeout)
            if line is None:
                return None
            m = json.loads(line)  # stdout must be pure JSON-RPC
            if m.get("id") == self.n:
                return m

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=20)
        except Exception:
            self.p.kill()


def mcp_phase(exe, state, roots, imgdir, model_id):
    print("== MCP phase (stdio, hcs.vision.analyze) ==")
    m = Mcp(exe, state, roots)
    ev = {}
    try:
        with open(os.path.join(imgdir, "red.png"), "wb") as f:
            f.write(red_square())
        with open(os.path.join(imgdir, "notimage.png"), "wb") as f:
            f.write(b"this is not an image, just bytes")
        r = m.tool("hcs.vision.analyze", {"model": model_id, "prompt": "What colour is the large square? One short sentence.", "image_path": os.path.join(imgdir, "red.png"), "max_tokens": 40, "temperature": 0})
        sc = r["result"].get("structuredContent", {}) if r else {}
        check("MCP vision.analyze returns an answer", r and not r["result"]["isError"] and sc.get("text", "").strip(), r)
        check("MCP answer mentions red", "red" in sc.get("text", "").lower(), sc.get("text"))
        ev["answer"] = sc.get("text")
        r = m.tool("hcs.vision.analyze", {"prompt": "x", "image_path": "C:\\Windows\\win.ini"})
        check("MCP: path outside approved roots -> security_violation", r and r["result"]["isError"] and r["result"]["structuredContent"]["error"]["code"] == "security_violation", r)
        r = m.tool("hcs.vision.analyze", {"prompt": "x", "image_path": os.path.join(imgdir, "..", "red.png")})
        check("MCP: '..' traversal -> security_violation", r and r["result"]["isError"] and r["result"]["structuredContent"]["error"]["code"] == "security_violation", r)
        r = m.tool("hcs.vision.analyze", {"model": model_id, "prompt": "x", "image_path": os.path.join(imgdir, "notimage.png")})
        check("MCP: non-image file -> invalid_argument", r and r["result"]["isError"] and r["result"]["structuredContent"]["error"]["code"] == "invalid_argument", r)
        r = m.tool("hcs.runtime.status", {})
        check("MCP still responsive afterwards", r and not r["result"]["isError"], r)
    finally:
        m.close()
    return ev


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--vlm", default=os.environ.get("HCS_TEST_VLM", ""))
    ap.add_argument("--evidence", default="")
    a = ap.parse_args()
    if not os.path.isfile(a.exe):
        print("BLOCKED: executable not found:", a.exe)
        return 77
    if not a.vlm or not os.path.isfile(a.vlm):
        print("BLOCKED (NOT RUN): HCS_TEST_VLM / --vlm not set or missing")
        return 77
    vdir = os.path.dirname(os.path.abspath(a.vlm))
    if not [f for f in os.listdir(vdir) if f.lower().startswith("mmproj") and f.lower().endswith(".gguf")]:
        print("BLOCKED (NOT RUN): no mmproj*.gguf next to the VLM")
        return 77
    state = tempfile.mkdtemp(prefix="hcs-e2e-vision-state-")
    imgdir = tempfile.mkdtemp(prefix="hcs-e2e-vision-img-")
    try:
        r = subprocess.run([a.exe, "--state-dir", state, "models", "scan", vdir, "--json"], capture_output=True, text=True)
        stem = os.path.splitext(os.path.basename(a.vlm))[0].lower()
        lst = subprocess.run([a.exe, "--state-dir", state, "models", "list", "--json"], capture_output=True, text=True)
        models = json.loads(lst.stdout)
        models = models.get("models", models) if isinstance(models, dict) else models
        model_id = next((m["id"] for m in models if m["id"] == stem or (stem in m["id"] and m.get("architecture") != "clip")), None)
        check("scan registers the VLM", model_id, lst.stdout[:300])
        if not model_id:
            return 1
        ev = {"schema_version": 1, "exe": os.path.basename(a.exe), "version": subprocess.run([a.exe, "--version"], capture_output=True, text=True).stdout.strip(),
              "vlm_file": os.path.basename(a.vlm), "vlm_bytes": os.path.getsize(a.vlm)}
        ev["http"] = http_phase(a.exe, state, [vdir], model_id, a.evidence)
        ev["mcp"] = mcp_phase(a.exe, state, [vdir, imgdir], imgdir, model_id)
        ev["checks"] = RESULTS
        ev["result_status"] = "PASS" if not FAILS else "FAIL"
        if a.evidence:
            os.makedirs(a.evidence, exist_ok=True)
            with open(os.path.join(a.evidence, "e2e-vision.json"), "w", encoding="utf-8") as f:
                json.dump(ev, f, indent=2)
        print("\nSUMMARY: %d checks, %d failed" % (len(RESULTS), len(FAILS)))
        for f in FAILS:
            print("  FAILED:", f)
        return 0 if not FAILS else 1
    finally:
        shutil.rmtree(state, ignore_errors=True)
        shutil.rmtree(imgdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
