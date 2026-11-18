"""Manual reproducer for soak defect D-S3 (Ctrl+C while generating). Windows only.

Starts `HCSInfer serve` in its OWN console, starts a long streamed chat, then delivers a real CTRL_C_EVENT to that
console (AttachConsole + GenerateConsoleCtrlEvent from this helper, which ignores the event itself) and reports how
long the server takes to exit. The parent first re-enables Ctrl+C handling: a process started from a shell that
disabled Ctrl+C hands the "ignore" flag to its children, which is why a naive os.kill(0, CTRL_C_EVENT) from a
harness is silently dropped (the original D-S3 symptom).

usage: python ctrl_c_manual.py <abs path to HCSInfer.exe> <model root>
"""
# Manual repro for D-S3: server in its own console, long generation, deliver a real CTRL_C_EVENT.
import ctypes, http.client, json, os, subprocess, sys, tempfile, time
exe, root = sys.argv[1], sys.argv[2]
state = tempfile.mkdtemp(prefix="hcs-ctrlc-")
log = os.path.join(state, "err.txt")
subprocess.run([exe, "--state-dir", state, "models", "scan", root], check=True, capture_output=True)
errf = open(log, "w")
CREATE_NEW_CONSOLE = 0x10
import ctypes as _c
print("Ctrl+C handling re-enabled for the child (call succeeded):", bool(_c.windll.kernel32.SetConsoleCtrlHandler(None, False)), flush=True)
p = subprocess.Popen([exe, "--state-dir", state, "serve", "--port", "0", "--model-root", root, "--timeout-s", "600"],
                     stdout=subprocess.DEVNULL, stderr=errf, creationflags=CREATE_NEW_CONSOLE)
port = tok = None
for _ in range(120):
    time.sleep(0.5)
    t = open(log, encoding="utf-8", errors="replace").read()
    for l in t.splitlines():
        if "listening on http://" in l: port = int(l.rsplit(":", 1)[1])
        if l.startswith("HCSInfer API token: "): tok = l.split(": ", 1)[1].strip()
    if port and tok: break
print("port", port, flush=True)
H = {"Authorization": "Bearer " + tok, "Content-Type": "application/json"}
c = http.client.HTTPConnection("127.0.0.1", port, timeout=120); c.request("GET", "/v1/models", headers=H)
models = json.loads(c.getresponse().read())["data"]
mid = [m["id"] for m in models if "lfm" in m["id"].lower() or "omni" in m["id"].lower()] or [models[0]["id"]]
mid = mid[0]; print("model", mid, flush=True)
import threading
def gen():
    try:
        cc = http.client.HTTPConnection("127.0.0.1", port, timeout=600)
        cc.request("POST", "/v1/chat/completions", json.dumps({"model": mid, "max_tokens": 4000, "stream": True,
            "messages": [{"role": "user", "content": "Write a very long story about a lighthouse."}]}), H)
        r = cc.getresponse(); n = 0
        while True:
            d = r.read(256)
            if not d: break
            n += 1
        print("client stream ended after", n, "reads", flush=True)
    except Exception as e: print("client:", e, flush=True)
threading.Thread(target=gen, daemon=True).start()
time.sleep(12)  # model load + generation running
k = ctypes.windll.kernel32
k.FreeConsole()
assert k.AttachConsole(p.pid), "AttachConsole failed %d" % k.GetLastError()
k.SetConsoleCtrlHandler(None, True)
t0 = time.time()
assert k.GenerateConsoleCtrlEvent(0, 0), "GenerateConsoleCtrlEvent failed"
try:
    rc = p.wait(timeout=60); print("EXIT rc=%s after %.1fs" % (rc, time.time() - t0))
except subprocess.TimeoutExpired:
    print("STILL RUNNING after 60s"); p.kill()
print(open(log, encoding="utf-8", errors="replace").read()[-400:])
