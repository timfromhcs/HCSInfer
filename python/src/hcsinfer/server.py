"""Start and stop a local ``HCSInfer serve`` child process."""
from __future__ import annotations

import os
import re
import secrets
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from types import TracebackType
from typing import List, Optional, Sequence, Type, Union

from ._exe import find_exe, run_cli
from .client import Client

_LISTEN = re.compile(r"listening on http://([^:\s]+):(\d+)")


class Server:
    """Context manager around ``HCSInfer serve --port 0``.

    The server binds loopback only, uses a random bearer token and a private state directory unless ``state_dir`` is
    given. ``model_roots`` are scanned (read-only) and approved before the server starts.
    """

    def __init__(self, model_roots: Sequence[Union[str, os.PathLike[str]]] = (), *, exe: Optional[Path] = None,
                 state_dir: Optional[str] = None, token: Optional[str] = None,
                 extra_args: Sequence[str] = (), startup_timeout: float = 60.0) -> None:
        self._exe = exe
        self._roots = [str(r) for r in model_roots]
        self._tmp: Optional[tempfile.TemporaryDirectory[str]] = None
        if state_dir is None:
            self._tmp = tempfile.TemporaryDirectory(prefix="hcsinfer-py-")
            state_dir = self._tmp.name
        self.state_dir = state_dir
        self.token = token or secrets.token_hex(24)
        self._extra = list(extra_args)
        self._startup_timeout = startup_timeout
        self._proc: Optional["subprocess.Popen[str]"] = None
        self._log: List[str] = []
        self.port: Optional[int] = None

    @property
    def url(self) -> str:
        if self.port is None:
            raise RuntimeError("server is not running")
        return f"http://127.0.0.1:{self.port}"

    @property
    def log(self) -> str:
        """Captured stderr of the server (the API token is never echoed here because we pass our own)."""
        return "".join(self._log)

    def client(self, timeout: float = 300.0) -> Client:
        return Client(self.url, token=self.token, timeout=timeout)

    def start(self) -> "Server":
        if self._proc is not None:
            raise RuntimeError("already started")
        exe = self._exe or find_exe()
        for root in self._roots:
            cp = run_cli(["models", "scan", root, "--json"], exe=exe, state_dir=self.state_dir, timeout=300)
            if cp.returncode != 0:
                raise RuntimeError(f"models scan {root!r} failed ({cp.returncode}): {cp.stderr.strip()[:300]}")
        cmd = [str(exe), "--state-dir", self.state_dir, "serve", "--port", "0", "--token", self.token]
        for root in self._roots:
            cmd += ["--model-root", root]
        cmd += self._extra
        flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
        self._proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                                      encoding="utf-8", errors="replace", creationflags=flags)
        found = threading.Event()

        def pump() -> None:
            assert self._proc is not None and self._proc.stderr is not None
            for line in self._proc.stderr:
                self._log.append(line)
                m = _LISTEN.search(line)
                if m and self.port is None:
                    self.port = int(m.group(2))
                    found.set()

        threading.Thread(target=pump, daemon=True).start()
        deadline = time.monotonic() + self._startup_timeout
        while not found.wait(0.1):
            if self._proc.poll() is not None or time.monotonic() > deadline:
                log = self.log
                self.stop()
                raise RuntimeError(f"HCSInfer server did not start: {log.strip()[:500]}")
        self.client(timeout=10).health()
        return self

    def stop(self, timeout: float = 15.0) -> None:
        proc, self._proc = self._proc, None
        if proc is not None and proc.poll() is None:
            try:
                import signal
                proc.send_signal(getattr(signal, "CTRL_BREAK_EVENT", signal.SIGTERM))
                proc.wait(timeout)
            except (subprocess.TimeoutExpired, OSError, ValueError):
                proc.kill()
                proc.wait(timeout)
        self.port = None
        if self._tmp is not None:
            try:
                self._tmp.cleanup()
            except OSError:
                pass
            self._tmp = None

    def __enter__(self) -> "Server":
        return self.start()

    def __exit__(self, exc_type: Optional[Type[BaseException]], exc: Optional[BaseException],
                 tb: Optional[TracebackType]) -> None:
        self.stop()
