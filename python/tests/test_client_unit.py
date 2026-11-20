"""Client tests against a tiny in-process stdlib HTTP server (protocol shape only; NOT inference evidence)."""
from __future__ import annotations

import base64
import json
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from typing import Any, Iterator, List

import pytest

from hcsinfer import Client, HcsError

SEEN: List[Any] = []


class _Handler(BaseHTTPRequestHandler):
    def log_message(self, *_: Any) -> None:
        pass

    def _send(self, status: int, obj: Any) -> None:
        raw = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self) -> None:  # noqa: N802
        SEEN.append((self.command, self.path, self.headers.get("Authorization")))
        if self.path == "/hcs/v1/health":
            self._send(200, {"status": "ok", "version": "x"})
        elif self.path == "/hcs/v1/models":
            self._send(200, {"models": [{"id": "m1"}]})
        elif self.path == "/hcs/v1/jobs/j1":
            self._send(200, {"id": "j1", "state": "succeeded"})
        else:
            self._send(401, {"error": {"code": "unauthorized", "message": "no", "request_id": "req-1"}})

    def do_POST(self) -> None:  # noqa: N802
        n = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(n) or b"{}")
        SEEN.append((self.command, self.path, body))
        if self.path == "/v1/chat/completions" and body.get("stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            for piece in ("Hel", "lo"):
                evt = {"choices": [{"delta": {"content": piece}}]}
                self.wfile.write(f"data: {json.dumps(evt)}\n\n".encode())
            self.wfile.write(b"data: [DONE]\n\n")
        elif self.path == "/v1/chat/completions":
            self._send(200, {"choices": [{"message": {"role": "assistant", "content": "Hello"}}]})
        elif self.path == "/v1/images/generations":
            self._send(200, {"data": [{"b64_json": base64.b64encode(b"PNGDATA").decode()}]})
        elif self.path == "/hcs/v1/models/m1/load":
            self._send(202, {"job_id": "j1"})
        else:
            self._send(404, {"error": {"code": "not_found", "message": "nope"}})


@pytest.fixture()
def client() -> Iterator[Client]:
    SEEN.clear()
    srv = HTTPServer(("127.0.0.1", 0), _Handler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    yield Client(f"http://127.0.0.1:{srv.server_port}", token="tok", timeout=10)
    srv.shutdown()
    srv.server_close()


def test_health_and_auth_header(client: Client) -> None:
    assert client.health()["status"] == "ok"
    assert client.models() == [{"id": "m1"}]
    assert SEEN[-1][2] == "Bearer tok"


def test_chat_variants(client: Client) -> None:
    assert client.chat_text("hi", max_tokens=4, seed=1) == "Hello"
    assert "".join(client.chat_stream("hi")) == "Hello"
    body = [s for s in SEEN if s[0] == "POST"][0][2]
    assert body["messages"] == [{"role": "user", "content": "hi"}]
    assert body["max_tokens"] == 4 and body["seed"] == 1 and body["stream"] is False


def test_image_load_and_errors(client: Client) -> None:
    assert client.image_generate("x") == b"PNGDATA"
    assert client.load_model("m1")["state"] == "succeeded"
    with pytest.raises(HcsError) as ei:
        client.runtime()
    assert ei.value.status == 401 and ei.value.code == "unauthorized" and ei.value.request_id == "req-1"


def test_rejects_non_http_url() -> None:
    with pytest.raises(ValueError):
        Client("https://example.com")


def test_find_exe_never_returns_own_entry_point(tmp_path, monkeypatch):  # type: ignore[no-untyped-def]
    """Regression: a PATH hit in this interpreter's Scripts dir (the ``hcsinfer`` console script) must be ignored."""
    import sys
    from pathlib import Path

    from hcsinfer import ExeNotFoundError, find_exe
    from hcsinfer import _exe

    scripts = tmp_path / "Scripts"
    scripts.mkdir()
    (scripts / "HCSInfer.exe").write_bytes(b"x")
    monkeypatch.setattr(_exe, "_own_script_dirs", lambda: [scripts.resolve()])
    monkeypatch.setattr(_exe, "_BIN", tmp_path / "nobin")
    monkeypatch.delenv("HCSINFER_EXE", raising=False)
    monkeypatch.setenv("PATH", str(scripts))
    try:
        found = find_exe()
    except ExeNotFoundError:
        found = None
    assert found is None
    other = tmp_path / "real"
    other.mkdir()
    (other / "HCSInfer.exe").write_bytes(b"x")
    monkeypatch.setenv("PATH", str(scripts) + __import__("os").pathsep + str(other))
    assert Path(str(find_exe())).parent == other.resolve() or Path(str(find_exe())).parent == other
    assert sys is not None
