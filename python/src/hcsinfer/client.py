"""Stdlib-only client for the HCSInfer local HTTP API (OpenAI-compatible routes plus /hcs/v1)."""
from __future__ import annotations

import base64
import http.client
import json
import time
import urllib.parse
from typing import Any, Dict, Iterator, List, Mapping, Optional, Union

JsonDict = Dict[str, Any]
Message = Mapping[str, Any]


class HcsError(RuntimeError):
    """The server answered with an error (``status`` is the HTTP status, ``code`` the stable HCS error code)."""

    def __init__(self, status: int, code: str, message: str, request_id: Optional[str] = None) -> None:
        super().__init__(f"HTTP {status} {code}: {message}")
        self.status = status
        self.code = code
        self.message = message
        self.request_id = request_id


class Client:
    """Talks to ``HCSInfer serve``. Uses only the standard library.

    >>> c = Client("http://127.0.0.1:8765", token="...")      # doctest: +SKIP
    >>> c.chat_text("Say hi", max_tokens=16)                   # doctest: +SKIP
    """

    def __init__(self, base_url: str = "http://127.0.0.1:8765", token: Optional[str] = None,
                 timeout: float = 300.0) -> None:
        parsed = urllib.parse.urlsplit(base_url)
        if parsed.scheme != "http" or not parsed.hostname:
            raise ValueError("base_url must be an http://host:port URL (HCSInfer serves plain HTTP on loopback)")
        self._host = parsed.hostname
        self._port = parsed.port or 80
        self.base_url = f"http://{self._host}:{self._port}"
        self.token = token
        self.timeout = timeout

    # -- transport ---------------------------------------------------------------------------------------------
    def _headers(self, body: bool) -> Dict[str, str]:
        h = {"Accept": "application/json"}
        if body:
            h["Content-Type"] = "application/json"
        if self.token:
            h["Authorization"] = f"Bearer {self.token}"
        return h

    def _open(self, method: str, path: str, payload: Optional[Mapping[str, Any]],
              timeout: Optional[float]) -> "tuple[http.client.HTTPConnection, http.client.HTTPResponse]":
        conn = http.client.HTTPConnection(self._host, self._port, timeout=timeout or self.timeout)
        data = json.dumps(payload).encode("utf-8") if payload is not None else None
        conn.request(method, path, body=data, headers=self._headers(data is not None))
        resp = conn.getresponse()
        if resp.status >= 400:
            raw = resp.read()
            conn.close()
            raise _to_error(resp.status, raw)
        return conn, resp

    def _json(self, method: str, path: str, payload: Optional[Mapping[str, Any]] = None,
              timeout: Optional[float] = None) -> JsonDict:
        conn, resp = self._open(method, path, payload, timeout)
        try:
            raw = resp.read()
        finally:
            conn.close()
        out = json.loads(raw.decode("utf-8")) if raw else {}
        if not isinstance(out, dict):
            raise HcsError(resp.status, "invalid_response", "server did not return a JSON object")
        return out

    # -- HCS-native routes -------------------------------------------------------------------------------------
    def health(self) -> JsonDict:
        """``GET /hcs/v1/health`` (no token needed)."""
        return self._json("GET", "/hcs/v1/health", timeout=10)

    def runtime(self) -> JsonDict:
        return self._json("GET", "/hcs/v1/runtime")

    def devices(self) -> JsonDict:
        return self._json("GET", "/hcs/v1/devices")

    def memory(self) -> JsonDict:
        return self._json("GET", "/hcs/v1/memory")

    def models(self) -> List[JsonDict]:
        """Registered models with lifecycle state (``GET /hcs/v1/models``)."""
        out = self._json("GET", "/hcs/v1/models")
        items = out.get("models", out.get("data", []))
        return [m for m in items if isinstance(m, dict)] if isinstance(items, list) else []

    def job(self, job_id: str) -> JsonDict:
        return self._json("GET", f"/hcs/v1/jobs/{urllib.parse.quote(job_id, safe='')}")

    def cancel_job(self, job_id: str) -> JsonDict:
        return self._json("POST", f"/hcs/v1/jobs/{urllib.parse.quote(job_id, safe='')}/cancel", {})

    def wait_job(self, job_id: str, timeout: float = 600.0, poll: float = 0.5) -> JsonDict:
        """Poll a job until it leaves ``queued``/``running``; raises ``HcsError`` if it failed or was cancelled."""
        deadline = time.monotonic() + timeout
        while True:
            j = self.job(job_id)
            state = j.get("state")
            if state == "succeeded":
                return j
            if state in ("failed", "cancelled"):
                raise HcsError(409, str(state), str(j.get("error") or f"job {state}"))
            if time.monotonic() > deadline:
                raise TimeoutError(f"job {job_id} still {state} after {timeout}s")
            time.sleep(poll)

    def load_model(self, model_id: str, timeout: float = 600.0) -> JsonDict:
        """Load a registered model and wait for the load job."""
        out = self._json("POST", f"/hcs/v1/models/{urllib.parse.quote(model_id, safe='')}/load", {})
        job_id = out.get("job_id")
        return self.wait_job(str(job_id), timeout) if job_id else out

    def unload_model(self, model_id: str) -> JsonDict:
        return self._json("POST", f"/hcs/v1/models/{urllib.parse.quote(model_id, safe='')}/unload", {})

    # -- OpenAI-compatible routes ------------------------------------------------------------------------------
    def openai_models(self) -> JsonDict:
        return self._json("GET", "/v1/models")

    def chat(self, messages: Union[str, List[Message]], *, model: Optional[str] = None,
             max_tokens: Optional[int] = None, temperature: Optional[float] = None,
             seed: Optional[int] = None, tools: Optional[List[Mapping[str, Any]]] = None) -> JsonDict:
        """Non-streaming ``POST /v1/chat/completions``; ``messages`` may be a plain prompt string."""
        body = _chat_body(messages, model, max_tokens, temperature, seed, False)
        if tools is not None:
            body["tools"] = list(tools)
        return self._json("POST", "/v1/chat/completions", body)

    def chat_text(self, prompt: str, **kwargs: Any) -> str:
        """Convenience wrapper returning only the assistant text."""
        out = self.chat(prompt, **kwargs)
        choices = out.get("choices") or [{}]
        content = (choices[0].get("message") or {}).get("content")
        return content if isinstance(content, str) else ""

    def chat_stream(self, messages: Union[str, List[Message]], *, model: Optional[str] = None,
                    max_tokens: Optional[int] = None, temperature: Optional[float] = None,
                    seed: Optional[int] = None) -> Iterator[str]:
        """Streaming chat: yields the ``content`` deltas. Closing the generator cancels the server-side job."""
        body = _chat_body(messages, model, max_tokens, temperature, seed, True)
        conn, resp = self._open("POST", "/v1/chat/completions", body, None)
        try:
            while True:
                line = resp.readline()
                if not line:
                    return
                text = line.decode("utf-8", errors="replace").strip()
                if not text.startswith("data:"):
                    continue
                data = text[5:].strip()
                if data == "[DONE]":
                    return
                evt = json.loads(data)
                if "error" in evt:
                    err = evt["error"]
                    raise HcsError(200, str(err.get("code", "stream_error")), str(err.get("message", "")))
                for ch in evt.get("choices", []):
                    piece = (ch.get("delta") or {}).get("content")
                    if piece:
                        yield str(piece)
        finally:
            conn.close()

    def image_generate(self, prompt: str, *, model: Optional[str] = None, size: str = "512x512",
                       steps: Optional[int] = None, seed: Optional[int] = None,
                       negative_prompt: Optional[str] = None) -> bytes:
        """``POST /v1/images/generations``; returns the PNG bytes."""
        body: JsonDict = {"prompt": prompt, "size": size}
        for k, v in (("model", model), ("steps", steps), ("seed", seed), ("negative_prompt", negative_prompt)):
            if v is not None:
                body[k] = v
        out = self._json("POST", "/v1/images/generations", body)
        data = out.get("data") or []
        if not data or "b64_json" not in data[0]:
            raise HcsError(200, "invalid_response", "no image in response")
        return base64.b64decode(data[0]["b64_json"])


def _chat_body(messages: Union[str, List[Message]], model: Optional[str], max_tokens: Optional[int],
               temperature: Optional[float], seed: Optional[int], stream: bool) -> JsonDict:
    msgs: List[Any] = [{"role": "user", "content": messages}] if isinstance(messages, str) else list(messages)
    body: JsonDict = {"messages": msgs, "stream": stream}
    for k, v in (("model", model), ("max_tokens", max_tokens), ("temperature", temperature), ("seed", seed)):
        if v is not None:
            body[k] = v
    return body


def _to_error(status: int, raw: bytes) -> HcsError:
    try:
        err = json.loads(raw.decode("utf-8")).get("error", {})
        return HcsError(status, str(err.get("code", "error")), str(err.get("message", "")), err.get("request_id"))
    except (ValueError, AttributeError):
        return HcsError(status, "error", raw.decode("utf-8", errors="replace")[:200])
