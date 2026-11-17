"""Development harness ONLY (not a test, not the product path): serves src/gui/assets at / and /ui/* and forwards every other
request to a real `HCSInfer serve`, so the UI can be exercised in a browser against real inference BEFORE
hcs::gui::serve is hooked into http_api.cpp. It re-creates the response headers the C++ code sends (CSP etc.) from the
same source constants only approximately; the authoritative header checks are the C++ unit tests and e2e_gui.py.

  python -I ui_proxy_dev.py --upstream 127.0.0.1:<port> [--listen-port 8899]
Open http://127.0.0.1:8899/#token=<token>
Streaming (SSE) responses are relayed unbuffered. The Origin header is dropped when forwarding (the browser talks to
this proxy's origin, the server only knows its own).
"""
import argparse
import http.client
import http.server
import os
import socketserver

ASSETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "src", "gui", "assets")
TYPES = {".html": "text/html; charset=utf-8", ".css": "text/css; charset=utf-8", ".js": "text/javascript; charset=utf-8"}
CSP = ("default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data: blob:; connect-src 'self'; font-src 'none'; "
       "media-src 'none'; object-src 'none'; frame-src 'none'; worker-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")


class H(http.server.BaseHTTPRequestHandler):
    upstream = ""
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def _asset(self, name):
        p = os.path.join(ASSETS, name)
        if not os.path.isfile(p) or os.path.splitext(name)[1] not in TYPES:
            self.send_error(404)
            return
        data = open(p, "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", TYPES[os.path.splitext(name)[1]])
        self.send_header("Content-Security-Policy", CSP)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _handle(self):
        path = self.path.split("?")[0]
        if self.command == "GET" and path == "/":
            return self._asset("index.html")
        if self.command == "GET" and path.startswith("/ui/") and "/" not in path[4:]:
            return self._asset(path[4:])
        host, port = self.upstream.split(":")
        c = http.client.HTTPConnection(host, int(port), timeout=600)
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else None
        h = {k: v for k, v in self.headers.items() if k.lower() in ("authorization", "content-type")}
        h["Host"] = self.upstream
        c.request(self.command, self.path, body=body, headers=h)
        r = c.getresponse()
        self.send_response(r.status)
        for k, v in r.getheaders():
            if k.lower() in ("content-type", "cache-control", "x-request-id", "allow", "retry-after"):
                self.send_header(k, v)
        self.end_headers()
        try:
            while True:
                chunk = r.read1(4096)
                if not chunk:
                    break
                self.wfile.write(chunk)
                self.wfile.flush()
        except (ConnectionError, OSError):
            pass  # browser went away: closing the upstream connection cancels the job exactly like a real client
        finally:
            c.close()

    do_GET = do_POST = do_DELETE = _handle


class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--upstream", required=True)
    ap.add_argument("--listen-port", type=int, default=8899)
    a = ap.parse_args()
    H.upstream = a.upstream
    print("proxy on http://127.0.0.1:%d -> %s" % (a.listen_port, a.upstream), flush=True)
    S(("127.0.0.1", a.listen_port), H).serve_forever()
