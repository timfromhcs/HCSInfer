"""HCS Studio visual + interaction evidence (docs/gui.md). Drives a real headless Edge/Chrome over the DevTools protocol
(stdlib + the `websockets` package) against a running `HCSInfer serve` (or ui_proxy_dev.py in front of one). Nothing is
mocked: it loads real models through the Models tab, streams a real chat reply, runs text-to-image and image-to-image
through the real routes, and captures a screenshot of every tab at desktop and phone width in light and dark.

  python -I studio_shots.py --base http://127.0.0.1:8791 --token <token> --out <dir>
        [--chat-model <id>] [--image-model <id>] [--no-interact]

Exit codes: 0 all executed checks pass, 1 a check failed, 77 BLOCKED (no browser / no websockets / server unreachable).
"""
import argparse
import base64
import http.client
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse

# `python -I` ignores PYTHONPATH; a dev-only venv (never committed) is made visible explicitly, as in e2e_gui.py.
_extra = os.environ.get("HCS_E2E_PYTHON_PATH", "")
if _extra and os.path.isdir(_extra) and _extra not in sys.path:
    sys.path.insert(0, _extra)
try:
    from websockets.sync.client import connect
except Exception:  # noqa: BLE001
    print("BLOCKED: python package 'websockets' (>= 11) is required")
    sys.exit(77)

TABS = ["chat", "image", "vision", "models", "perf", "settings", "agent", "train"]
RESULTS, FAILS = [], []


def check(name, cond, detail=""):
    RESULTS.append({"check": name, "status": "PASS" if cond else "FAIL", "detail": str(detail)[:300]})
    print(("PASS  " if cond else "FAIL  ") + name + ((" -- " + str(detail)[:200]) if not cond and detail else ""))
    if not cond:
        FAILS.append(name)


def find_browser():
    for c in (os.path.join(os.environ.get("ProgramFiles(x86)", ""), "Microsoft", "Edge", "Application", "msedge.exe"),
              os.path.join(os.environ.get("ProgramFiles", ""), "Microsoft", "Edge", "Application", "msedge.exe"),
              os.path.join(os.environ.get("ProgramFiles", ""), "Google", "Chrome", "Application", "chrome.exe")):
        if os.path.isfile(c):
            return c
    for n in ("msedge", "chrome", "google-chrome", "chromium"):
        p = shutil.which(n)
        if p:
            return p
    return None


class Page:
    def __init__(self, ws_url):
        self.ws = connect(ws_url, max_size=64 * 1024 * 1024)  # sync client; closed with the browser
        self.n = 0
        self.problems = []  # console errors / exceptions / CSP violations

    def call(self, method, **params):
        self.n += 1
        mid = self.n
        self.ws.send(json.dumps({"id": mid, "method": method, "params": params}))
        while True:
            msg = json.loads(self.ws.recv(timeout=120))
            if msg.get("id") == mid:
                if "error" in msg:
                    raise RuntimeError(method + ": " + str(msg["error"]))
                return msg.get("result", {})
            self._event(msg)

    def _event(self, msg):
        m, p = msg.get("method"), msg.get("params", {})
        if m == "Runtime.consoleAPICalled" and p.get("type") in ("error", "warning"):
            self.problems.append("console." + p["type"] + ": " + " ".join(str(a.get("value", a.get("description", ""))) for a in p.get("args", [])))
        elif m == "Runtime.exceptionThrown":
            self.problems.append("exception: " + p.get("exceptionDetails", {}).get("text", "") + " " + str(p.get("exceptionDetails", {}).get("exception", {}).get("description", "")))
        elif m == "Log.entryAdded" and p["entry"].get("level") in ("error", "warning"):
            self.problems.append("log." + p["entry"]["level"] + ": " + p["entry"].get("text", "") + " " + p["entry"].get("url", ""))

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                self._event(json.loads(self.ws.recv(timeout=0.2)))
            except TimeoutError:
                pass

    def js(self, expr):
        r = self.call("Runtime.evaluate", expression=expr, returnByValue=True, awaitPromise=True)
        if "exceptionDetails" in r:
            raise RuntimeError("js: " + str(r["exceptionDetails"])[:300])
        return r["result"].get("value")

    def wait(self, expr, timeout=60, what=""):
        end = time.time() + timeout
        while time.time() < end:
            if self.js(expr):
                return True
            self.pump(0.25)
        print("   timeout waiting for", what or expr)
        return False

    def viewport(self, w, h, mobile):
        self.call("Emulation.setDeviceMetricsOverride", width=w, height=h, deviceScaleFactor=2 if mobile else 1, mobile=mobile)
        self.pump(0.4)  # let the sidebar transition settle before anything is captured

    def scheme(self, s):
        self.call("Emulation.setEmulatedMedia", features=[{"name": "prefers-color-scheme", "value": s}])

    def shot(self, path, full=False):
        t0 = time.time()
        r = self.call("Page.captureScreenshot", format="png", captureBeyondViewport=full)
        if time.time() - t0 > 5:
            print("   slow screenshot %.1fs: %s" % (time.time() - t0, os.path.basename(path)))
        with open(path, "wb") as f:
            f.write(base64.b64decode(r["data"]))

    def tab(self, name):
        self.js("document.getElementById('tab-%s').click()" % name)
        self.pump(0.5)


def api_get(base, token, path):
    u = urllib.parse.urlparse(base)
    c = http.client.HTTPConnection(u.hostname, u.port, timeout=15)
    c.request("GET", path, headers={"Authorization": "Bearer " + token})
    r = c.getresponse()
    return r.status, json.loads(r.read() or b"null")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--token", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--chat-model", default="")
    ap.add_argument("--image-model", default="")
    ap.add_argument("--no-interact", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    browser = find_browser()
    if not browser:
        print("BLOCKED: no Edge/Chrome found")
        return 77
    try:
        st, models = api_get(a.base, a.token, "/hcs/v1/models")
    except Exception as e:  # noqa: BLE001
        print("BLOCKED: server unreachable:", e)
        return 77
    prof = tempfile.mkdtemp(prefix="hcs-studio-shots-")
    import socket
    sk = socket.socket()
    sk.bind(("127.0.0.1", 0))
    port = sk.getsockname()[1]
    sk.close()
    proc = subprocess.Popen([browser, "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
                             "--disable-renderer-backgrounding", "--disable-background-timer-throttling",
                             "--disable-backgrounding-occluded-windows", "--disable-extensions",
                             f"--remote-debugging-port={port}", f"--user-data-dir={prof}", "about:blank"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        ws_url = None
        for _ in range(60):
            try:
                c = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
                c.request("GET", "/json")
                tabs = json.loads(c.getresponse().read())
                ws_url = next(t["webSocketDebuggerUrl"] for t in tabs if t.get("type") == "page" and t.get("url") == "about:blank")
                break
            except Exception:  # noqa: BLE001
                time.sleep(0.5)
        if not ws_url:
            print("BLOCKED: browser did not expose a DevTools endpoint")
            return 77
        pg = Page(ws_url)
        for d in ("Page", "Runtime", "Log"):
            pg.call(d + ".enable")
        pg.call("Emulation.setFocusEmulationEnabled", enabled=True)
        pg.viewport(1360, 860, False)
        pg.scheme("light")
        pg.call("Page.navigate", url=f"{a.base}/#token={a.token}")
        ok = pg.wait("document.getElementById('conn') && document.getElementById('conn').classList.contains('pill-ok')", 30, "connected")
        check("UI connected with the fragment token", ok)
        check("title is HCS Studio", pg.js("document.title.indexOf('HCS Studio') >= 0"), pg.js("document.title"))
        check("token removed from the address bar", "token" not in pg.js("location.href"))
        check("token not in DOM", a.token not in pg.js("document.documentElement.outerHTML"))
        check("inline SVG wordmark rendered", pg.js("!!document.querySelector('.wordmark svg text')"))

        # Planned tabs: real status text, no controls.
        for t in ("agent", "train"):
            pg.tab(t)
            n_ctl = pg.js("document.querySelectorAll('#panel-%s button, #panel-%s input, #panel-%s select, #panel-%s textarea').length" % ((t,) * 4))
            txt = pg.js("document.getElementById('panel-%s').textContent" % t)
            check("%s tab is labelled Planned and has no controls" % t, n_ctl == 0 and "Planned" in txt, n_ctl)

        # Layout checks at several widths: no horizontal page scroll on any tab.
        for label, w, h, mobile in (("desktop", 1360, 860, False), ("tablet", 820, 1100, True), ("phone", 390, 844, True)):
            pg.viewport(w, h, mobile)
            for t in TABS:
                pg.tab(t)
                over = pg.js("document.documentElement.scrollWidth - document.documentElement.clientWidth")
                check("no horizontal page scroll: %s/%s" % (label, t), over <= 1, over)

        for scheme in ("light", "dark"):
            pg.scheme(scheme)
            for label, w, h, mobile in (("desktop", 1360, 860, False), ("phone", 390, 844, True)):
                pg.viewport(w, h, mobile)
                for t in TABS:
                    pg.tab(t)
                    pg.shot(os.path.join(a.out, f"{label}-{scheme}-{t}.png"), full=(t in ("perf",)))
        pg.scheme("light")
        pg.viewport(1360, 860, False)
        pg.tab("chat")

        if not a.no_interact:
            ids = [m["id"] for m in models.get("models", [])]
            chat_id = a.chat_model or next((i for i in ids if "mmproj" not in i and any(m["id"] == i and m.get("kind") in ("text", "vision") for m in models["models"])), "")
            img_id = a.image_model or next((m["id"] for m in models["models"] if m.get("kind") == "image"), "")
            if chat_id:
                pg.tab("models")
                pg.js("document.querySelectorAll('#models-table tbody tr').forEach(function(tr){ if ((tr.firstChild.textContent === %s || tr.firstChild.lastChild.textContent === %s)) { tr.querySelector('button').click(); } })" % (json.dumps(chat_id), json.dumps(chat_id)))
                ok = pg.wait("document.getElementById('loaded').textContent.indexOf(%s) >= 0" % json.dumps(chat_id), 240, "model loaded")
                check("model loaded through the Models tab", ok, pg.js("document.getElementById('loaded').textContent"))
                pg.pump(1.0)
                pg.shot(os.path.join(a.out, "desktop-light-models-loaded.png"))
                check("memory split shows segments after load", pg.js("document.querySelectorAll('#mem-bar span').length") >= 1, pg.js("document.getElementById('mem-note').textContent"))
                pg.tab("chat")
                pg.js("var i=document.getElementById('chat-input'); i.value='Reply with one short sentence about the sea.'; document.getElementById('chat-form').requestSubmit()")
                pg.wait("document.querySelector('.msg-assistant .msg-body') && document.querySelector('.msg-assistant .msg-body').textContent.length > 0", 180, "first token")
                done = pg.wait("!document.getElementById('chat-send').disabled", 180, "chat done")
                reply = pg.js("(document.querySelector('.msg-assistant .msg-body')||{textContent:''}).textContent")
                check("streamed chat reply shown", done and len(reply.strip()) > 0, reply[:80])
                pg.shot(os.path.join(a.out, "desktop-light-chat-reply.png"))
                # Stop button
                pg.js("document.getElementById('set-forget') && 0")
                pg.js("document.querySelector('input[name=mode][value=advanced]').click()")
                pg.js("document.getElementById('chat-max').value='1024'; document.getElementById('chat-input').value='Write a very long story about a lighthouse keeper.'; document.getElementById('chat-form').requestSubmit()")
                pg.wait("document.querySelectorAll('.msg-assistant .msg-body')[1] && document.querySelectorAll('.msg-assistant .msg-body')[1].textContent.length > 20", 120, "story started")
                pg.js("document.getElementById('chat-stop').click()")
                check("Stop ends generation and re-enables Send", pg.wait("!document.getElementById('chat-send').disabled", 60, "stop"))
                pg.js("document.querySelector('input[name=mode][value=simple]').click()")
            else:
                print("NOT RUN: no text model registered")
            if img_id:
                pg.tab("image")
                pg.js("var s=document.getElementById('image-model'); s.value=%s; s.dispatchEvent(new Event('change'))" % json.dumps(img_id))
                pg.js("Array.from(document.querySelectorAll('input[name=image-preset]')).find(function(r){return r.value==='20'}).click(); document.getElementById('image-size').value='512x512'; document.getElementById('image-prompt').value='a red apple on a wooden table, studio light'; document.getElementById('image-seed').value='42'")
                pg.js("document.getElementById('image-form').requestSubmit()")
                pg.pump(2)
                check("busy bar shown while rendering", pg.js("!document.getElementById('image-busy').hidden") or pg.js("document.getElementById('image-gallery').children.length") >= 1)
                pg.shot(os.path.join(a.out, "desktop-light-image-working.png"))
                ok = pg.wait("document.getElementById('image-gallery').children.length >= 1", 600, "text-to-image")
                state = pg.js("document.getElementById('image-state').textContent")
                if not ok and ("Unsupported" in state or "Out of memory" in state):
                    check("text-to-image reports the server's state, never a fake image", True, state)
                else:
                    check("text-to-image produced an image", ok, state)
                    check("image has real pixels", pg.js("document.getElementById('image-out').naturalWidth") >= 64, pg.js("document.getElementById('image-out').naturalWidth"))
                    pg.shot(os.path.join(a.out, "desktop-light-image-txt2img.png"))
                    pg.js("document.getElementById('image-reuse').click()")
                    pg.js("document.getElementById('image-strength').value='0.6'; document.getElementById('image-strength').dispatchEvent(new Event('input')); document.getElementById('image-prompt').value='the same apple, watercolor painting'")
                    check("img2img mode with input preview", pg.js("!document.getElementById('image-init-box').hidden && !document.getElementById('image-init-preview').hidden"))
                    pg.js("document.getElementById('image-form').requestSubmit()")
                    ok2 = pg.wait("document.getElementById('image-gallery').children.length >= 2", 600, "image-to-image")
                    check("image-to-image produced a second image", ok2, pg.js("document.getElementById('image-state').textContent"))
                    pg.shot(os.path.join(a.out, "desktop-light-image-img2img.png"))
                    pg.viewport(390, 844, True)
                    pg.shot(os.path.join(a.out, "phone-light-image-result.png"), full=True)
                    pg.viewport(1360, 860, False)
            else:
                print("NOT RUN: no image model registered")
        pg.pump(0.5)
        csp = [p for p in pg.problems if "Content Security Policy" in p or "Refused to" in p]
        check("no CSP violations logged", not csp, csp[:2])
        other = [p for p in pg.problems if p not in csp]
        check("no console errors or exceptions", not other, other[:3])
        pg.call("Emulation.clearDeviceMetricsOverride")
    finally:
        proc.kill()
        proc.wait()
        shutil.rmtree(prof, ignore_errors=True)
    summary = {"schema_version": 1, "checks": RESULTS, "result_status": "FAIL" if FAILS else "PASS"}
    with open(os.path.join(a.out, "studio-shots.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)
    print("\nSUMMARY: %d checks, %d failed -> %s" % (len(RESULTS), len(FAILS), summary["result_status"]))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
