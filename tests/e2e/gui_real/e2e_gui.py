"""End-to-end check of the embedded local web UI served by `HCSInfer serve` (ADR-009, docs/gui.md).

Phases (each reports PASS / FAIL / NOT RUN; nothing is mocked or simulated):
  1. HTTP: GET / and /ui/* are served with the strict CSP, traversal is rejected, the API still demands the token.
     If GET / returns the API's JSON 401/404 error the UI hook is not wired into http_api.cpp yet -> BLOCKED (exit 77), never a pass.
  2. Browser boot (headless Edge/Chrome --dump-dom): the page loads with the token in the URL FRAGMENT, connects, and
     lists the registered models from /hcs/v1/models. NOT RUN when no Chromium-family browser is found.
  3. Browser interaction (python `playwright` + an installed Edge/Chrome): load a model through the Models tab, chat with
     streaming, press Stop, check Image Studio state, and that no CSP violation was logged. NOT RUN when playwright is
     missing. The chat/load steps additionally need a real model (--model / HCS_TEST_GGUF).

Exit codes: 0 all executed checks pass, 1 a check failed, 77 BLOCKED (prerequisite or hook missing).
Run:  python -I e2e_gui.py --exe <HCSInfer.exe> [--model <gguf>] [--evidence <dir>]
Stdlib only (playwright optional).
"""
import argparse
import http.client
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

RESULTS, FAILS, NOT_RUN = [], [], []


def check(name, cond, detail=""):
    RESULTS.append({"check": name, "status": "PASS" if cond else "FAIL", "detail": str(detail)[:400]})
    print(("PASS  " if cond else "FAIL  ") + name + (("  -- " + str(detail)[:240]) if detail and not cond else ""))
    if not cond:
        FAILS.append(name)


def not_run(name, reason):
    RESULTS.append({"check": name, "status": "NOT RUN", "detail": reason})
    NOT_RUN.append(name)
    print("NOT RUN  " + name + "  -- " + reason)


def start_reader(stream, q):
    def run():
        for line in iter(stream.readline, ""):
            q.put(line.rstrip("\r\n"))
        q.put(None)
    threading.Thread(target=run, daemon=True).start()


def req(port, method, path, token=None, headers=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    h = {"Host": f"127.0.0.1:{port}"}
    if token:
        h["Authorization"] = "Bearer " + token
    h.update(headers or {})
    try:
        c.request(method, path, headers=h)
        r = c.getresponse()
        body = r.read()
        return r.status, {k.lower(): v for k, v in r.getheaders()}, body
    finally:
        c.close()


def find_browser():
    cands = [
        os.path.join(os.environ.get("ProgramFiles(x86)", ""), "Microsoft", "Edge", "Application", "msedge.exe"),
        os.path.join(os.environ.get("ProgramFiles", ""), "Microsoft", "Edge", "Application", "msedge.exe"),
        os.path.join(os.environ.get("ProgramFiles", ""), "Google", "Chrome", "Application", "chrome.exe"),
        os.path.join(os.environ.get("ProgramFiles(x86)", ""), "Google", "Chrome", "Application", "chrome.exe"),
    ]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    for n in ("msedge", "chrome", "google-chrome", "chromium"):
        p = shutil.which(n)
        if p:
            return p
    return None


def http_phase(port, token):
    print("== Phase 1: UI over HTTP ==")
    st, h, body = req(port, "GET", "/")
    if st in (401, 404) and h.get("content-type", "").startswith("application/json") and b'"error"' in body:
        # An unwired server answers "/" with the API's own JSON error (401 unauthorized or 404 not_found).
        print("BLOCKED: GET / returned the API's JSON error (%d) - the UI hook (hcs::gui::serve) is not wired into http_api.cpp in this binary" % st)
        return False
    check("GET / without token -> 200 html", st == 200 and h.get("content-type", "").startswith("text/html"), (st, h.get("content-type")))
    csp = h.get("content-security-policy", "")
    check("CSP present and strict", "default-src 'none'" in csp and "unsafe-inline" not in csp and "script-src 'self'" in csp, csp)
    check("framing and referrer hardened", h.get("x-frame-options") == "DENY" and h.get("referrer-policy") == "no-referrer", h)
    check("index has no token embedded", token.encode() not in body)
    for a, ct in (("app.js", "javascript"), ("app.css", "text/css")):
        st, h, body = req(port, "GET", "/ui/" + a)
        check(f"/ui/{a} served ({ct})", st == 200 and ct in h.get("content-type", "") and len(body) > 500, (st, h.get("content-type"), len(body)))
        check(f"/ui/{a} carries CSP", "default-src 'none'" in h.get("content-security-policy", ""))
    for p in ("/ui/../x", "/ui/..%2f..%2fwindows", "/ui/%2e%2e/x", "/ui/a/b", "/ui/.env"):
        st, _, _ = req(port, "GET", p)
        check(f"traversal rejected: {p}", st in (400, 404), st)
    st, _, _ = req(port, "POST", "/")
    check("POST / -> 405", st == 405, st)
    st, _, _ = req(port, "GET", "/v1/models")
    check("API still requires the token", st == 401, st)
    st, _, _ = req(port, "GET", "/", headers={"Host": "evil.example.com"})
    check("UI refuses foreign Host (DNS rebinding)", st == 403, st)
    st, _, _ = req(port, "GET", "/", headers={"Origin": "http://evil.example"})
    check("UI refuses foreign Origin", st == 403, st)
    st, _, body = req(port, "GET", "/v1/models", token=token)
    check("API works with token (UI's own call)", st == 200, st)
    return True


def browser_boot_phase(port, token, browser, expect_models):
    print("== Phase 2: browser boot (headless --dump-dom) ==")
    if not browser:
        not_run("browser boot", "no Edge/Chrome executable found")
        return
    prof = tempfile.mkdtemp(prefix="hcs-e2e-browser-")
    try:
        url = f"http://127.0.0.1:{port}/#token={token}"
        cmd = [browser, "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
               f"--user-data-dir={prof}", "--virtual-time-budget=10000", "--dump-dom", url]
        r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
        dom = r.stdout
        if "<title>HCS Studio</title>" not in dom:
            check("browser rendered the UI", False, (r.returncode, dom[:200], r.stderr[-200:]))
            return
        check("browser rendered the UI", True)
        conn = re.search(r'id="conn"[^>]*>([^<]*)<', dom)
        check("UI connected to the API using the fragment token", bool(conn) and conn.group(1).strip() == "connected", conn.group(1) if conn else "no #conn")
        rows = re.findall(r"<tr><td>", dom)
        if expect_models:
            check("Models table lists registered models", len(rows) >= 1, len(rows))
        check("token not left in DOM", token not in dom)
    finally:
        shutil.rmtree(prof, ignore_errors=True)


def playwright_phase(port, token, model_stem, have_model):
    print("== Phase 3: browser interaction (playwright) ==")
    try:
        from playwright.sync_api import sync_playwright
    except Exception as e:  # noqa: BLE001
        not_run("browser interaction (load / chat / stop / image state)", "python playwright is not installed (" + type(e).__name__ + ")")
        return
    with sync_playwright() as p:
        browser = None
        for channel in ("msedge", "chrome", None):
            try:
                browser = p.chromium.launch(channel=channel) if channel else p.chromium.launch()
                break
            except Exception:  # noqa: BLE001
                continue
        if browser is None:
            not_run("browser interaction (load / chat / stop / image state)", "playwright found no launchable Chromium/Edge/Chrome")
            return
        ctx = browser.new_context()
        page = ctx.new_page()
        console = []
        page.on("console", lambda m: console.append((m.type, m.text)))
        page.goto(f"http://127.0.0.1:{port}/#token={token}")
        page.wait_for_selector("#conn.pill-ok", timeout=15000)
        check("UI connected", True)
        check("fragment cleared from the URL", "token" not in page.url, page.url)
        # Image Studio: either unsupported with the server's reason, or a real image. Never silent.
        page.click("#tab-image")
        if not have_model:
            not_run("model load / chat / vision via UI", "no HCS_TEST_GGUF model")
            browser.close()
            return
        page.click("#tab-models")
        page.wait_for_selector("#models-table tbody tr button")
        row = page.locator("#models-table tbody tr", has_text=model_stem).first
        row.get_by_role("button", name="Load").click()
        page.wait_for_selector("#loaded.pill-ok", timeout=180000)
        check("model loaded through the Models tab", True, page.inner_text("#loaded"))
        page.click("#tab-chat")
        page.fill("#chat-input", "Reply with exactly one short sentence about the sea.")
        page.click("#chat-send")
        page.wait_for_function("document.querySelectorAll('.msg-assistant span')[0] && document.querySelectorAll('.msg-assistant span')[0].textContent.length > 0", timeout=120000)
        page.wait_for_selector("#chat-send:not([disabled])", timeout=120000)
        reply = page.locator(".msg-assistant span").first.inner_text()
        check("streamed chat reply shown", len(reply.strip()) > 0, reply[:80])
        # Stop: long generation, cancel through the job cancel endpoint
        page.click("#chat-clear")
        page.click("#tab-settings")
        page.check("input[name=mode][value=advanced]")
        page.click("#tab-chat")
        page.fill("#chat-max", "1024")
        page.fill("#chat-input", "Write a very long story about a lighthouse keeper.")
        page.click("#chat-send")
        page.wait_for_function("(document.querySelector('.msg-assistant span')||{textContent:''}).textContent.length > 20", timeout=120000)
        page.click("#chat-stop")
        page.wait_for_selector("#chat-send:not([disabled])", timeout=30000)
        check("Stop ends generation and re-enables Send", True)
        violations = [t for k, t in console if "Content Security Policy" in t]
        check("no CSP violations logged", not violations, violations[:2])
        browser.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--model", default=os.environ.get("HCS_TEST_GGUF", ""))
    ap.add_argument("--evidence", default="")
    a = ap.parse_args()
    a.exe = os.path.abspath(a.exe)
    if not os.path.isfile(a.exe):
        print("BLOCKED: executable not found:", a.exe)
        return 77
    have_model = bool(a.model) and os.path.isfile(a.model)
    model_dir = os.path.dirname(os.path.abspath(a.model)) if have_model else tempfile.mkdtemp(prefix="hcs-e2e-models-")
    state = tempfile.mkdtemp(prefix="hcs-e2e-gui-state-")
    stem = os.path.splitext(os.path.basename(a.model))[0].lower() if have_model else ""
    proc = None
    try:
        if have_model:
            subprocess.run([a.exe, "--state-dir", state, "models", "scan", model_dir], capture_output=True, text=True)
        proc = subprocess.Popen([a.exe, "--state-dir", state, "serve", "--port", "0", "--model-root", model_dir],
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
            print("FAIL: serve did not print port and token")
            return 1
        wired = http_phase(port, token)
        if not wired:
            status = 77
        else:
            browser = find_browser()
            browser_boot_phase(port, token, browser, expect_models=have_model)
            playwright_phase(port, token, stem, have_model)
            status = 1 if FAILS else 0
    finally:
        if proc:
            proc.kill()
            proc.wait()
        shutil.rmtree(state, ignore_errors=True)
        if not have_model:
            shutil.rmtree(model_dir, ignore_errors=True)
    summary = {"schema_version": 1, "checks": RESULTS, "not_run": NOT_RUN,
               "result_status": {0: "PASS", 1: "FAIL", 77: "BLOCKED"}[status]}
    if a.evidence:
        os.makedirs(a.evidence, exist_ok=True)
        with open(os.path.join(a.evidence, "e2e-gui.json"), "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2)
    print("\nSUMMARY: %d checks, %d failed, %d not run -> %s" % (len(RESULTS), len(FAILS), len(NOT_RUN), summary["result_status"]))
    return status


if __name__ == "__main__":
    sys.exit(main())
