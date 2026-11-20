"""Markdown-safety + copy/perf/onboarding evidence for HCS Studio (docs/gui.md), real browser, real model.

Drives headless Edge/Chrome over DevTools (stdlib + `websockets`) against a running `HCSInfer serve`. It asks the real
model to echo Markdown and an HTML payload, then checks in the DOM that:
  - Markdown became elements (strong/code/list/code block with a Copy button),
  - the HTML payload was shown as text and never became an element (no <img>, no <script>, no handler),
  - the Performance tab shows a measured tokens/s after the chat,
  - no console errors / CSP violations were logged.

  python -I studio_md.py --base http://127.0.0.1:8791 --token <token> --model <id> --out <dir>
Exit codes: 0 pass, 1 a check failed, 77 BLOCKED (no browser / websockets / server).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import studio_shots as ss  # reuse Page / find_browser / check
except SystemExit:
    raise


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--token", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    browser = ss.find_browser()
    if not browser:
        print("BLOCKED: no Edge/Chrome")
        return 77
    import http.client
    import socket
    sk = socket.socket()
    sk.bind(("127.0.0.1", 0))
    port = sk.getsockname()[1]
    sk.close()
    prof = tempfile.mkdtemp(prefix="hcs-studio-md-")
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
        pg.scheme("light")
        pg.call("Page.navigate", url=f"{a.base}/#token={a.token}")
        ss.check("connected", pg.wait("document.getElementById('conn').classList.contains('pill-ok')", 30))
        ss.check("first-run guide visible", pg.js("!document.getElementById('onboard').hidden"))
        # load the model through the Models tab
        pg.tab("models")
        pg.js("document.querySelectorAll('#models-table tbody tr').forEach(function(tr){ if (tr.firstChild.lastChild.textContent === %s || tr.firstChild.textContent === %s) tr.querySelector('button').click(); })" % (json.dumps(a.model), json.dumps(a.model)))
        ss.check("model loaded", pg.wait("document.getElementById('loaded').textContent.indexOf(%s) >= 0" % json.dumps(a.model), 240))
        pg.tab("chat")
        prompt = ("Repeat the following text exactly, with nothing before or after it:\n"
                  "## Heading\n**bold word** and `inline code`\n- one\n- two\n```python\nprint(1)\n```\n"
                  "<img src=x onerror=document.title='pwned'><script>document.title='pwned'</script>")
        pg.js("var i=document.getElementById('chat-input'); i.value=%s; document.getElementById('chat-form').requestSubmit()" % json.dumps(prompt))
        pg.wait("document.querySelector('.msg-assistant .msg-body') && document.querySelector('.msg-assistant .msg-body').textContent.length > 0", 180)
        done = pg.wait("!document.getElementById('chat-send').disabled", 240)
        ss.check("reply finished", done)
        body = "document.querySelector('.msg-assistant .msg-body')"
        pg.shot(os.path.join(a.out, "desktop-light-chat-markdown.png"))
        raw = pg.js("%s.raw || ''" % body)
        ss.check("model echoed the markdown payload (precondition)", "**bold word**" in raw or "bold word" in raw, raw[:120])
        ss.check("no <img>/<script>/handler element created from model output",
                 pg.js("%s.querySelectorAll('img,script,iframe,object,embed,[onerror],[onclick]').length" % body) == 0)
        ss.check("page title unchanged (no script ran)", pg.js("document.title.indexOf('pwned')") == -1)
        if "**bold word**" in raw:
            ss.check("bold rendered as <strong>", pg.js("%s.querySelectorAll('strong').length" % body) >= 1)
        if "```" in raw:
            ss.check("code block rendered with a Copy button", pg.js("%s.querySelectorAll('.codeblock .copy').length" % body) >= 1)
        if "<img" in raw:
            ss.check("HTML payload shown as text", "<img" in pg.js("%s.textContent" % body))
        # copy button gives feedback (clipboard may be refused in headless; the toast must say which)
        pg.js("document.querySelector('.msg-assistant .copy').click()")
        pg.pump(0.6)
        ss.check("copy button reports a result", pg.js("!document.getElementById('toast').hidden"), pg.js("document.getElementById('toast').textContent"))
        # perf shows measured speed
        pg.tab("perf")
        pg.wait("document.getElementById('perf-tps').textContent !== '-'", 20)
        tps = pg.js("document.getElementById('perf-tps').textContent")
        ss.check("Performance shows measured tokens/s after chat", tps not in ("", "-"), tps)
        pg.shot(os.path.join(a.out, "desktop-light-perf-live.png"))
        # theme toggle: light -> dark via the explicit control
        pg.js("document.querySelector('input[name=theme][value=dark]').click()")
        ss.check("manual dark theme applies", pg.js("document.documentElement.getAttribute('data-theme')") == "dark")
        pg.js("document.querySelector('input[name=theme][value=light]').click()")
        pg.js("document.getElementById('set-reset').click()")
        ss.check("reset restores system theme + guide", pg.js("!document.documentElement.hasAttribute('data-theme') && !document.getElementById('onboard').hidden"))
        pg.pump(0.5)
        csp = [p for p in pg.problems if "Content Security Policy" in p or "Refused to" in p]
        ss.check("no CSP violations", not csp, csp[:2])
        other = [p for p in pg.problems if p not in csp]
        ss.check("no console errors", not other, other[:3])
    finally:
        proc.kill()
        proc.wait()
        shutil.rmtree(prof, ignore_errors=True)
    with open(os.path.join(a.out, "studio-md.json"), "w", encoding="utf-8") as f:
        json.dump({"schema_version": 1, "checks": ss.RESULTS, "result_status": "FAIL" if ss.FAILS else "PASS"}, f, indent=2)
    print("SUMMARY: %d checks, %d failed" % (len(ss.RESULTS), len(ss.FAILS)))
    return 1 if ss.FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
