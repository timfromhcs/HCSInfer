"""Real-engine end-to-end test of image generation/editing over HTTP (`HCSInfer serve`, via curl.exe) and MCP
(`HCSInfer mcp stdio`, scripted JSON-RPC client), plus the embedded web UI hook. No mocks: needs a real diffusion
GGUF (HCS_TEST_DIFFUSION_MODEL, e.g. SD1.5 Q8_0) and a binary built with the stable-diffusion.cpp engine.
Exit codes: 0 PASS, 1 FAIL, 77 BLOCKED (missing prerequisite; ctest SKIP).

Run:  python -I e2e_image.py --exe <HCSInfer.exe> [--model <gguf>] [--evidence <dir>] [--size 256] [--steps 2]
Stdlib only. A generated PNG is verified structurally (signature, IHDR size, non-blank pixel data) and kept in the
evidence directory; it is NOT judged for aesthetic quality (2 steps at 256x256 is a smoke test).
"""
import argparse
import base64
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


def png_info(data):
    """Returns (width, height, distinct_byte_values_in_pixel_data) or None when `data` is not a PNG."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos, idat, w, h = 8, b"", 0, 0
    while pos + 8 <= len(data):
        n, t = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if t == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif t == b"IDAT":
            idat += body
        pos += 12 + n
    try:
        raw = zlib.decompress(idat)
    except zlib.error:
        return None
    return w, h, len(set(raw))


class Curl:
    def __init__(self, base, token):
        self.base, self.token = base, token
        self.tmp = tempfile.mkdtemp(prefix="hcs-e2e-img-curl-")

    def req(self, method, path, body=None, token=True, headers=None, timeout=900, raw_body=None, head=False):
        hdr = os.path.join(self.tmp, "h.txt")
        out = os.path.join(self.tmp, "b.txt")
        if os.path.exists(out):
            os.remove(out)
        cmd = ["curl.exe", "-s", "-S", "-m", str(timeout), "-D", hdr, "-o", out, "-w", "%{http_code}"]
        cmd += ["-I"] if head else ["-X", method]
        if token:
            cmd += ["-H", "Authorization: Bearer " + self.token]
        for k, v in (headers or {}).items():
            cmd += ["-H", f"{k}: {v}"]
        data = raw_body if raw_body is not None else (json.dumps(body) if body is not None else None)
        if data is not None:
            dfile = os.path.join(self.tmp, "d.json")
            with open(dfile, "w", encoding="utf-8") as f:
                f.write(data)
            if not any(k.lower() == "content-type" for k in (headers or {})):
                cmd += ["-H", "Content-Type: application/json"]
            cmd += ["--data-binary", "@" + dfile]
        cmd.append(self.base + path)
        p = subprocess.run(cmd, capture_output=True, text=True)
        code = p.stdout.strip()
        text = open(out, encoding="utf-8", errors="replace").read() if os.path.exists(out) else ""
        hdrs = open(hdr, encoding="utf-8", errors="replace").read() if os.path.exists(hdr) else ""
        return (int(code) if code.isdigit() else 0), text, hdrs, p.stderr.strip()

    def jreq(self, *a, **k):
        code, text, hdrs, err = self.req(*a, **k)
        try:
            return code, (json.loads(text) if text else None)
        except ValueError:
            return code, {"_raw": text[:300], "_err": err}


class McpClient:
    def __init__(self, exe, state_dir, roots):
        cmd = [exe, "--state-dir", state_dir, "mcp", "stdio"]
        for r in roots:
            cmd += ["--model-root", r]
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, encoding="utf-8", bufsize=1)
        self.out = queue.Queue()
        start_reader(self.p.stdout, self.out)
        start_reader(self.p.stderr, queue.Queue())
        self.nid = 0

    def send(self, obj):
        self.p.stdin.write(json.dumps(obj) + "\n")
        self.p.stdin.flush()

    def request(self, method, params=None, timeout=900):
        self.nid += 1
        params = dict(params or {})
        params["_meta"] = {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
                           "io.modelcontextprotocol/clientInfo": {"name": "e2e", "version": "1"},
                           "io.modelcontextprotocol/clientCapabilities": {}}
        rid = self.nid
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        while True:
            line = self.out.get(timeout=timeout)
            if line is None:
                return None
            msg = json.loads(line)
            if msg.get("id") == rid:
                return msg

    def tool(self, name, args=None, timeout=900):
        return self.request("tools/call", {"name": name, "arguments": args or {}}, timeout=timeout)

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=30)
        except Exception:
            self.p.kill()


def start_server(exe, state, roots):
    cmd = [exe, "--state-dir", state, "serve", "--port", "0"]
    for r in roots:
        cmd += ["--model-root", r]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
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
    return proc, port, token


def http_phase(exe, model_dir, model_stem, state, evidence, size, steps):
    print("== HTTP phase (serve + curl.exe) ==")
    ev = {}
    proc, port, token = start_server(exe, state, [model_dir])
    check("serve prints port and token", port and token, (port, bool(token)))
    if not (port and token):
        proc.kill()
        return ev
    try:
        c = Curl(f"http://127.0.0.1:{port}", token)

        # --- embedded UI hook -----------------------------------------------------------------
        code, text, hdrs, _ = c.req("GET", "/", token=False)
        check("UI: GET / without token -> 200 html", code == 200 and "<html" in text.lower(), (code, text[:80]))
        check("UI: CSP header present", "content-security-policy" in hdrs.lower() and "default-src 'none'" in hdrs, hdrs[:300])
        check("UI: page does not contain the API token", token not in text)
        code, text, hdrs, _ = c.req("GET", "/ui/app.js", token=False)
        check("UI: /ui/app.js served", code == 200 and len(text) > 1000, code)
        check("UI: app.js does not contain the API token", token not in text)
        code, _, _, _ = c.req("GET", "/", token=False, headers={"Host": "evil.example.com"})
        check("UI: DNS-rebinding Host -> 403", code == 403, code)
        code, _, _, _ = c.req("GET", "/", token=False, headers={"Origin": "http://evil.example.com"})
        check("UI: foreign Origin -> 403", code == 403, code)
        code, _, _, _ = c.req("POST", "/", token=False, body={})
        check("UI: POST / -> 405", code == 405, code)
        code, _, hdrs, _ = c.req("GET", "/", head=True, token=False)
        check("UI: HEAD / -> 200", code == 200, code)
        code, _ = c.jreq("GET", "/v1/models", token=False)
        check("API still needs the token next to the UI", code == 401, code)

        # --- models -----------------------------------------------------------------------------
        code, j = c.jreq("GET", "/hcs/v1/models")
        models = (j or {}).get("models", [])
        mid = next((m["id"] for m in models if model_stem in m["id"]), None)
        mk = next((m for m in models if m["id"] == mid), {})
        check("registry lists the diffusion model with kind=image", mid is not None and mk.get("kind") == "image", mk)
        if mid is None:
            return ev
        code, j = c.jreq("GET", "/hcs/v1/runtime")
        check("runtime reports an available image engine", code == 200 and j["image_engine"]["available"] is True, j and j.get("image_engine"))

        # --- validation errors over the wire (engine is never touched) ---------------------------------
        code, j = c.jreq("POST", "/v1/images/generations", {"model": mid, "prompt": "x", "size": "250x256"})
        check("size not multiple of 8 -> 400", code == 400, (code, j))
        code, j = c.jreq("POST", "/v1/images/generations", {"model": mid, "prompt": "x", "steps": 999})
        check("steps over cap -> 400", code == 400, (code, j))
        code, j = c.jreq("POST", "/v1/images/generations", {"model": mid, "prompt": "x", "size": "4096x4096"})
        check("oversized image -> 400", code == 400, (code, j))
        code, j = c.jreq("POST", "/v1/images/generations", {"prompt": "x", "size": "256x256"})
        check("no model loaded and none requested -> 409", code == 409, (code, j))
        code, j = c.jreq("POST", "/v1/images/generations", token=False, body={"prompt": "x"})
        check("images route needs the token -> 401", code == 401, code)

        # --- load (real, asynchronous job) -------------------------------------------------------------
        t = time.time()
        code, j = c.jreq("POST", f"/hcs/v1/models/{mid}/load")
        check("load accepted as a job (202)", code == 202 and "job_id" in (j or {}), (code, j))
        job = (j or {}).get("job_id")
        state_s = None
        while job and time.time() - t < 1500:
            _, st = c.jreq("GET", f"/hcs/v1/jobs/{job}")
            state_s = (st or {}).get("state")
            if state_s in ("succeeded", "failed", "cancelled"):
                break
            time.sleep(1.0)
        ev["load_wall_s"] = round(time.time() - t, 2)
        check("model load job succeeded", state_s == "succeeded", (state_s, st))
        if state_s != "succeeded":
            return ev

        # --- text-to-image ---------------------------------------------------------------------------
        body = {"model": mid, "prompt": "a simple red apple on a plain table", "size": f"{size}x{size}", "steps": steps, "seed": 7}
        t = time.time()
        code, j = c.jreq("POST", "/v1/images/generations", body)
        ev["generate_wall_s"] = round(time.time() - t, 2)
        check("POST /v1/images/generations -> 200", code == 200 and j and j.get("data"), (code, str(j)[:200]))
        gen_png = None
        if code == 200 and j and j.get("data"):
            gen_png = base64.b64decode(j["data"][0]["b64_json"])
            info = png_info(gen_png)
            check("generated image is a PNG of the requested size", info is not None and info[:2] == (size, size), info)
            check("generated image is not blank (pixel data has variety)", info is not None and info[2] > 20, info)
            hcs = j.get("hcs", {})
            check("response reports job id, steps and timing", hcs.get("steps") == steps and hcs.get("generation_ms", 0) > 0, hcs)
            ev["generation"] = {"hcs": hcs, "png_bytes": len(gen_png), "png_info": info}
            if evidence:
                os.makedirs(evidence, exist_ok=True)
                with open(os.path.join(evidence, f"http-generate-{size}x{size}-{steps}steps.png"), "wb") as f:
                    f.write(gen_png)
            _, st = c.jreq("GET", f"/hcs/v1/jobs/{hcs.get('job_id')}")
            check("job status reports the image result", st and st.get("state") == "succeeded" and st.get("result", {}).get("kind") == "image", st)
            t = time.time()
            code2, j2 = c.jreq("POST", "/v1/images/generations", body)
            ev["generate_again_wall_s"] = round(time.time() - t, 2)
            same = code2 == 200 and j2["data"][0]["b64_json"] == j["data"][0]["b64_json"]
            ev["same_seed_identical_bytes"] = bool(same)  # informational: reproducibility is not asserted

        # --- edit ------------------------------------------------------------------------------------
        if gen_png:
            uri = "data:image/png;base64," + base64.b64encode(gen_png).decode()
            t = time.time()
            code, j = c.jreq("POST", "/v1/images/edits", {"model": mid, "prompt": "a simple green apple on a plain table", "image": uri,
                                                          "steps": steps, "strength": 0.6, "seed": 7})
            ev["edit_wall_s"] = round(time.time() - t, 2)
            check("POST /v1/images/edits -> 200", code == 200 and j and j.get("data"), (code, str(j)[:200]))
            if code == 200 and j and j.get("data"):
                ep = base64.b64decode(j["data"][0]["b64_json"])
                info = png_info(ep)
                check("edited image is a PNG of the input size", info is not None and info[:2] == (size, size), info)
                check("edited image differs from the input", ep != gen_png)
                if evidence:
                    with open(os.path.join(evidence, f"http-edit-{size}x{size}-{steps}steps.png"), "wb") as f:
                        f.write(ep)
            code, j = c.jreq("POST", "/v1/images/edits", {"model": mid, "prompt": "x", "image": "data:image/png;base64,AAAA"})
            check("edit with a non-image -> 400", code == 400, (code, j))
            code, j = c.jreq("POST", "/v1/images/edits", {"model": mid, "prompt": "x", "image": "file:///c:/windows/win.ini"})
            check("edit with a file URL -> 400 (no server paths)", code == 400, (code, j))
            multipart = "\n".join([
                "--x",
                'Content-Disposition: form-data; name="prompt"',
                "",
                "hello",
                "--x--",
                "",
            ])
            code, _, _, _ = c.req("POST", "/v1/images/edits", raw_body=multipart, headers={"Content-Type": "multipart/form-data; boundary=x"})
            check("edit with multipart -> 415", code == 415, code)

        # --- cancellation: a client that disconnects must cancel the render ----------------------------
        long_body = {"model": mid, "prompt": "a long render", "size": "512x512", "steps": 60, "seed": 1}
        t = time.time()
        code, _, _, err = c.req("POST", "/v1/images/generations", body=long_body, timeout=4)
        check("client gives up on a long render (curl timeout)", code == 0, (code, err[:80]))
        t = time.time()
        code, j = c.jreq("POST", "/v1/images/generations", body, timeout=600)
        ev["after_disconnect_wall_s"] = round(time.time() - t, 2)
        # 60 steps at 512x512 on this machine is far longer than a 256x256 2-step image: finishing promptly proves the
        # abandoned job was cancelled and the engine is free again.
        check("next request completes promptly after the disconnect (abandoned job cancelled)",
              code == 200 and ev["after_disconnect_wall_s"] < 120, (code, ev["after_disconnect_wall_s"]))
        _, rt = c.jreq("GET", "/hcs/v1/runtime")
        ev["runtime_image_engine"] = rt.get("image_engine") if rt else None
        code, j = c.jreq("POST", f"/hcs/v1/models/{mid}/unload")
        check("unload image model", code == 200, (code, j))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except Exception:
            proc.kill()
    return ev


def mcp_phase(exe, model_dir, model_stem, state, evidence, size, steps):
    print("== MCP phase (mcp stdio, scripted client) ==")
    ev = {}
    work = tempfile.mkdtemp(prefix="hcs-e2e-img-in-")  # second approved root for the edit input
    m = McpClient(exe, state, [model_dir, work])
    try:
        r = m.request("tools/list")
        names = [t["name"] for t in r["result"]["tools"]]
        check("MCP tools/list includes the image tools", "hcs.image.generate" in names and "hcs.image.edit" in names, names)
        r = m.tool("hcs.models.list")
        models = r["result"]["structuredContent"]["models"]
        mid = next((x["id"] for x in models if model_stem in x["id"]), None)
        check("MCP models.list finds the diffusion model", mid is not None, [x["id"] for x in models][:5])
        if mid is None:
            return ev
        r = m.tool("hcs.image.generate", {"model": mid, "prompt": "x", "width": 250, "height": 256})
        check("MCP generate with a bad size -> invalid_argument", r["result"]["isError"] and r["result"]["structuredContent"]["error"]["code"] == "invalid_argument", r)
        t = time.time()
        r = m.tool("hcs.image.generate", {"model": mid, "prompt": "a simple red apple on a plain table", "width": size, "height": size,
                                          "steps": steps, "seed": 7})
        ev["generate_wall_s"] = round(time.time() - t, 2)
        ok = r and r["result"]["isError"] is False
        check("MCP hcs.image.generate (real) succeeds", ok, str(r)[:300])
        png = None
        if ok:
            blk = r["result"]["content"][0]
            check("MCP result carries an image content block", blk.get("type") == "image" and blk.get("mimeType") == "image/png", blk.get("type"))
            png = base64.b64decode(blk["data"])
            info = png_info(png)
            check("MCP image is a PNG of the requested size", info is not None and info[:2] == (size, size), info)
            check("MCP image is not blank", info is not None and info[2] > 20, info)
            sc = r["result"]["structuredContent"]
            check("MCP structuredContent has metadata only", "data" not in sc and sc.get("steps") == steps, sc)
            ev["generation"] = sc
            if evidence:
                os.makedirs(evidence, exist_ok=True)
                with open(os.path.join(evidence, f"mcp-generate-{size}x{size}-{steps}steps.png"), "wb") as f:
                    f.write(png)
        if png:
            inp = os.path.join(work, "input.png")
            with open(inp, "wb") as f:
                f.write(png)
            t = time.time()
            r = m.tool("hcs.image.edit", {"model": mid, "prompt": "a simple green apple on a plain table", "input_path": inp,
                                          "steps": steps, "strength": 0.6, "seed": 7})
            ev["edit_wall_s"] = round(time.time() - t, 2)
            ok = r and r["result"]["isError"] is False
            check("MCP hcs.image.edit (real) succeeds", ok, str(r)[:300])
            if ok:
                ep = base64.b64decode(r["result"]["content"][0]["data"])
                info = png_info(ep)
                check("MCP edited image is a PNG of the input size", info is not None and info[:2] == (size, size), info)
                if evidence:
                    with open(os.path.join(evidence, f"mcp-edit-{size}x{size}-{steps}steps.png"), "wb") as f:
                        f.write(ep)
            outside = os.path.join(tempfile.gettempdir(), "hcs-e2e-outside.png")
            with open(outside, "wb") as f:
                f.write(png)
            try:
                r = m.tool("hcs.image.edit", {"model": mid, "prompt": "x", "input_path": outside})
                check("MCP edit input outside approved roots -> security_violation",
                      r["result"]["isError"] and r["result"]["structuredContent"]["error"]["code"] == "security_violation", r)
            finally:
                os.remove(outside)
            r = m.tool("hcs.image.edit", {"model": mid, "prompt": "x", "input_path": os.path.join(work, "..", "..", "windows", "win.ini")})
            check("MCP edit input with .. -> security_violation",
                  r["result"]["isError"] and r["result"]["structuredContent"]["error"]["code"] == "security_violation", r)

        # cancellation of a long real render over MCP
        m.nid += 1
        rid = m.nid
        params = {"name": "hcs.image.generate", "arguments": {"model": mid, "prompt": "a long render", "width": 512, "height": 512, "steps": 60},
                  "_meta": {"io.modelcontextprotocol/protocolVersion": "2026-07-28", "io.modelcontextprotocol/clientCapabilities": {}}}
        m.send({"jsonrpc": "2.0", "id": rid, "method": "tools/call", "params": params})
        time.sleep(4.0)
        m.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": rid, "reason": "e2e"}})
        t = time.time()
        r = m.tool("hcs.image.generate", {"model": mid, "prompt": "x", "width": size, "height": size, "steps": steps, "seed": 7}, timeout=600)
        ev["after_cancel_wall_s"] = round(time.time() - t, 2)
        check("MCP: next render completes promptly after cancelling the long one",
              r and r["result"]["isError"] is False and ev["after_cancel_wall_s"] < 120, (ev["after_cancel_wall_s"], str(r)[:200]))
    finally:
        m.close()
        shutil.rmtree(work, ignore_errors=True)
    return ev


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--model", default=os.environ.get("HCS_TEST_DIFFUSION_MODEL", ""))
    ap.add_argument("--evidence", default="")
    ap.add_argument("--size", type=int, default=256)
    ap.add_argument("--steps", type=int, default=2)
    a = ap.parse_args()
    if not os.path.isfile(a.exe):
        print("BLOCKED: executable not found:", a.exe)
        return 77
    if not a.model or not os.path.isfile(a.model):
        print("BLOCKED: HCS_TEST_DIFFUSION_MODEL not set / model missing")
        return 77
    if shutil.which("curl.exe") is None and shutil.which("curl") is None:
        print("BLOCKED: curl.exe not found")
        return 77
    probe = subprocess.run([a.exe, "image", "--help"], capture_output=True, text=True)
    if probe.returncode != 0:
        print("BLOCKED: this binary has no diffusion engine")
        return 77
    model_dir = os.path.dirname(os.path.abspath(a.model))
    stem = os.path.splitext(os.path.basename(a.model))[0].lower()
    state = tempfile.mkdtemp(prefix="hcs-e2e-img-state-")
    try:
        r = subprocess.run([a.exe, "--state-dir", state, "models", "scan", model_dir], capture_output=True, text=True)
        print(r.stdout.strip().splitlines()[0] if r.stdout.strip() else r.stderr.strip())
        v = subprocess.run([a.exe, "--version"], capture_output=True, text=True).stdout.strip()
        ev = {"schema_version": 1, "exe": os.path.basename(a.exe), "version": v, "model_file": os.path.basename(a.model),
              "model_size_bytes": os.path.getsize(a.model), "size": a.size, "steps": a.steps}
        ev["http"] = http_phase(a.exe, model_dir, stem, state, a.evidence, a.size, a.steps)
        ev["mcp"] = mcp_phase(a.exe, model_dir, stem, state, a.evidence, a.size, a.steps)
        ev["checks"] = RESULTS
        ev["result_status"] = "PASS" if not FAILS else "FAIL"
        if a.evidence:
            os.makedirs(a.evidence, exist_ok=True)
            with open(os.path.join(a.evidence, "e2e-image.json"), "w", encoding="utf-8") as f:
                json.dump(ev, f, indent=2)
        print("\nSUMMARY: %d checks, %d failed" % (len(RESULTS), len(FAILS)))
        for f in FAILS:
            print("  FAILED:", f)
        return 0 if not FAILS else 1
    finally:
        shutil.rmtree(state, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
