"""One-off probe for the D-S2 handle observation: floods `serve` (no model needed) with 70 idle/half-open sockets
six times and prints the server handle count after each round. usage: python handle_probe.py <abs HCSInfer.exe>"""
import ctypes, ctypes.wintypes as wt, http.client, os, socket, subprocess, sys, tempfile, time
exe = sys.argv[1]
state = tempfile.mkdtemp(prefix="hcs-h-")
log = os.path.join(state, "e.txt"); ef = open(log, "w")
p = subprocess.Popen([exe, "--state-dir", state, "serve", "--port", "0"], stdout=subprocess.DEVNULL, stderr=ef)
port = None
for _ in range(60):
    time.sleep(0.25)
    for l in open(log, encoding="utf-8", errors="replace").read().splitlines():
        if "listening on http://" in l: port = int(l.rsplit(":", 1)[1])
    if port: break
k = ctypes.windll.kernel32
k.OpenProcess.restype = wt.HANDLE
h = k.OpenProcess(0x1000, False, p.pid)
def hc():
    n = wt.DWORD(0); k.GetProcessHandleCount(h, ctypes.byref(n)); return n.value
def health():
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=20); c.request("GET", "/hcs/v1/health"); r = c.getresponse(); r.read(); c.close(); return r.status
print("start", hc(), health(), hc())
for rnd in range(6):
    socks = []
    for i in range(70):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=5)
            if i % 2: s.sendall(b"POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 100000\r\n\r\n{")
            socks.append(s)
        except Exception: break
    time.sleep(0.5)
    st = health()
    for s in socks: s.close()
    time.sleep(3)
    print("round", rnd, "served", len(socks), "health", st, "handles", hc(), flush=True)
p.kill()
