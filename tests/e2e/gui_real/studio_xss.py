"""Runtime XSS regression for HCS Studio (docs/gui.md), real browser, FAKE model reply.

The real shipped UI assets (src/gui/assets) are served by a tiny stdlib stub that also answers the few /hcs/v1/* calls the
UI makes on start-up and streams a hostile "model reply" over SSE from /v1/chat/completions. The reply contains HTML/JS
payloads (<img onerror>, <script>, <svg onload>, javascript: link, an unclosed fence). A headless Edge/Chrome (DevTools,
stdlib + `websockets`) then checks in the live DOM that none of it became an element, no handler ran, and no CSP
violation / console error was logged. This is a UI-sanitisation test only: it proves nothing about inference.
The real-model variant is studio_md.py (needs a loaded model; not part of CTest).

  python -I studio_xss.py --out <dir>
Exit codes: 0 pass, 1 a check failed, 77 BLOCKED (no browser / websockets).
"""
import argparse
import http.client
import http.server
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import studio_shots as ss  # noqa: E402  (exits 77 itself when `websockets` is missing)

ASSETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "src", "gui", "assets")
TYPES = {".html": "text/html; charset=utf-8", ".css": "text/css; charset=utf-8", ".js": "text/javascript; charset=utf-8"}
# Same policy the C++ server sends (kept in sync with ui_proxy_dev.py; the authoritative header test is test_gui_assets.cpp).
CSP = ("default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data: blob:; connect-src 'self'; font-src 'none'; "
       "media-src 'none'; object-src 'none'; frame-src 'none'; worker-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")
PAYLOAD = ("## Heading\n**bold word** and `inline code`\n"
           "<img src=x onerror=\"document.title='pwned-img'\">\n"
           "<script>document.title='pwned-script'</script>\n"
           "<svg onload=\"document.title='pwned-svg'\"></svg>\n"
           "[click](javascript:document.title='pwned-link')\n"
           "<iframe src=\"javascript:document.title='pwned-iframe'\"></iframe>\n"
           "```html\n<img src=x onerror=alert(1)>\n```\n"
           "```js\nunclosed fence <script>1</script>")
TOKEN = "stub-token"


class Stub(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Security-Policy", CSP)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj):
        self._send(200, json.dumps(obj).encode(), "application/json")

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/":
            path = "/ui/index.html"
        if path.startswith("/ui/") and "/" not in path[4:]:
            p = os.path.join(ASSETS, path[4:])
            ext = os.path.splitext(p)[1]
            if os.path.isfile(p) and ext in TYPES:
                return self._send(200, open(p, "rb").read(), TYPES[ext])
            return self._send(404, b"{}", "application/json")
        if path == "/hcs/v1/health":
            return self._json({"status": "ok", "version": "stub"})
        if path == "/hcs/v1/runtime":
            return self._json({"loaded_model": "stub-model", "backend": "cpu"})
        if path == "/hcs/v1/models":
            return self._json({"models": [{"id": "stub-model", "capabilities": ["text"]}]})
        if path in ("/hcs/v1/memory", "/hcs/v1/devices"):
            return self._json({})
        return self._send(404, b"{}", "application/json")

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        self.rfile.read(n)
        if self.headers.get("Authorization") != "Bearer " + TOKEN:
            return self._send(401, b'{"error":{"code":"unauthorized","message":"bad token"}}', "application/json")
        if self.path.split("?")[0] != "/v1/chat/completions":
            return self._send(404, b"{}", "application/json")
        events = []
        step = 7  # split into small deltas so the streaming re-render path is exercised too
        for i in range(0, len(PAYLOAD), step):
            events.append({"id": "chatcmpl-1", "choices": [{"delta": {"content": PAYLOAD[i:i + step]}}]})
        events.append({"id": "chatcmpl-1", "choices": [{"delta": {}, "finish_reason": "stop"}]})
        body = "".join("data: " + json.dumps(e) + "\n\n" for e in events) + "data: [DONE]\n\n"
        self._send(200, body.encode(), "text/event-stream")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    browser = ss.find_browser()
    if not browser:
        print("BLOCKED: no Edge/Chrome")
        return 77
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Stub)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d" % srv.server_address[1]
    sk = socket.socket()
    sk.bind(("127.0.0.1", 0))
    port = sk.getsockname()[1]
    sk.close()
    prof = tempfile.mkdtemp(prefix="hcs-studio-xss-")
    proc = subprocess.Popen([browser, "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
                             "--disable-extensions", f"--remote-debugging-port={port}", f"--user-data-dir={prof}",
                             "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        ws_url = None
        for _ in range(60):
            try:
                c = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
                c.request("GET", "/json")
                tabs = json.loads(c.getresponse().read())
                ws_url = next(t["webSocketDebuggerUrl"] for t in tabs if t.get("type") == "page")
                break
            except Exception:  # noqa: BLE001
                time.sleep(0.5)
        if not ws_url:
            print("BLOCKED: no DevTools endpoint")
            return 77
        pg = ss.Page(ws_url)
        for d in ("Page", "Runtime", "Log"):
            pg.call(d + ".enable")
        pg.call("Emulation.setFocusEmulationEnabled", enabled=True)
        pg.viewport(1360, 860, False)
        pg.call("Page.navigate", url=f"{base}/#token={TOKEN}")
        ss.check("connected", pg.wait("document.getElementById('conn').classList.contains('pill-ok')", 30))
        pg.tab("chat")
        pg.js("var i=document.getElementById('chat-input'); i.value='go'; document.getElementById('chat-form').requestSubmit()")
        pg.wait("document.querySelector('.msg-assistant .msg-body') && document.querySelector('.msg-assistant .msg-body').textContent.length > 0", 30)
        ss.check("reply finished", pg.wait("!document.getElementById('chat-send').disabled", 30))
        pg.pump(0.5)
        body = "document.querySelector('.msg-assistant .msg-body')"
        raw = pg.js("%s.raw || ''" % body)
        ss.check("precondition: hostile payload reached the UI intact", "<script>" in raw and "onerror" in raw, raw[:80])
        ss.check("no img/script/svg/iframe/object/embed/handler element created",
                 pg.js("%s.querySelectorAll('img,script,svg,iframe,object,embed,[onerror],[onload],[onclick]').length" % body) == 0)
        ss.check("no javascript: href created", pg.js("Array.prototype.some.call(%s.querySelectorAll('a'), function(a){return /^\\s*javascript:/i.test(a.getAttribute('href')||'')})" % body) is False)
        ss.check("page title unchanged (no payload ran)", pg.js("document.title.indexOf('pwned')") == -1)
        ss.check("payload shown as text", "<img" in pg.js("%s.textContent" % body))
        ss.check("markdown still rendered (bold + code block)",
                 pg.js("%s.querySelectorAll('strong').length" % body) >= 1 and pg.js("%s.querySelectorAll('.codeblock').length" % body) >= 1)
        pg.pump(0.5)
        ss.check("no console errors / CSP violations", not pg.problems, "; ".join(pg.problems)[:300])
        res = {"tool": "studio_xss.py", "fake_model_reply": True, "browser": os.path.basename(browser),
               "checks": ss.RESULTS, "result_status": "PASS" if not ss.FAILS else "FAIL"}
        with open(os.path.join(a.out, "studio-xss.json"), "w") as f:
            json.dump(res, f, indent=2)
        return 1 if ss.FAILS else 0
    finally:
        proc.terminate()
        srv.shutdown()


if __name__ == "__main__":
    sys.exit(main())
