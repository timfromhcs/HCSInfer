"""Tests against the REAL HCSInfer executable (HCSINFER_EXE or a bundled exe) and a real GGUF (HCS_TEST_GGUF).

Skipped (= NOT RUN) when the executable or the model is unavailable.
"""
from __future__ import annotations

import os
from pathlib import Path
from typing import Optional

import pytest

import hcsinfer
from hcsinfer import Client, ExeNotFoundError, Server, doctor, find_exe, run_cli

EXE: Optional[Path]
try:
    EXE = find_exe()
except ExeNotFoundError:
    EXE = None

GGUF = os.environ.get("HCS_TEST_GGUF", "")
needs_exe = pytest.mark.skipif(EXE is None, reason="no HCSInfer executable (set HCSINFER_EXE)")
needs_model = pytest.mark.skipif(EXE is None or not Path(GGUF).is_file(), reason="HCS_TEST_GGUF not set / missing")


@needs_exe
def test_version_and_help() -> None:
    assert hcsinfer.version().strip()
    cp = run_cli(["--help"])
    assert cp.returncode == 0 and "serve" in cp.stdout


@needs_exe
def test_doctor_json() -> None:
    rep = doctor()
    assert isinstance(rep, dict) and rep


@needs_exe
def test_server_health_without_model() -> None:
    with Server() as srv:
        assert srv.client().health()["status"] == "ok"


@needs_model
def test_real_chat_against_gguf() -> None:
    root = str(Path(GGUF).parent)
    with Server([root], extra_args=["--timeout-s", "240"]) as srv:
        c: Client = srv.client(timeout=300)
        ids = [m["id"] for m in c.models() if Path(GGUF).stem.lower().replace("_", "-") in str(m.get("id", "")).lower()
               or str(m.get("id", "")).lower() in Path(GGUF).stem.lower()]
        models = c.models()
        assert models, "no model registered after scan"
        mid = ids[0] if ids else str(models[0]["id"])
        text = c.chat_text("Reply with the single word: pong", model=mid, max_tokens=24, temperature=0.0, seed=1)
        assert isinstance(text, str) and text.strip()
        streamed = "".join(c.chat_stream("Say hi", model=mid, max_tokens=12, seed=1))
        assert streamed.strip()
