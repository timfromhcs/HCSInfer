"""Locating and running the HCSInfer executable."""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence

_BIN = Path(__file__).resolve().parent / "bin"


class ExeNotFoundError(FileNotFoundError):
    """No HCSInfer executable could be located."""


def _vulkan_loader_present() -> bool:
    root = os.environ.get("SystemRoot")
    return bool(root) and (Path(str(root)) / "System32" / "vulkan-1.dll").is_file()


def find_exe(prefer: Optional[str] = None) -> Path:
    """Return the HCSInfer executable.

    Order: ``HCSINFER_EXE`` (explicit); the bundled ``bin/HCSInfer-vulkan.exe`` when the system Vulkan loader is
    present and ``prefer`` / ``HCSINFER_BACKEND`` is not ``cpu``; the bundled CPU ``bin/HCSInfer.exe``; ``HCSInfer``
    on PATH.
    """
    explicit = os.environ.get("HCSINFER_EXE")
    if explicit:
        p = Path(explicit)
        if p.is_file():
            return p
        raise ExeNotFoundError(f"HCSINFER_EXE points to a missing file: {explicit}")
    backend = (prefer or os.environ.get("HCSINFER_BACKEND") or "auto").lower()
    if backend not in ("auto", "cpu", "vulkan"):
        raise ValueError("backend must be 'auto', 'cpu' or 'vulkan'")
    vk, cpu = _BIN / "HCSInfer-vulkan.exe", _BIN / "HCSInfer.exe"
    if backend == "vulkan" and vk.is_file():
        return vk
    if backend == "auto" and vk.is_file() and _vulkan_loader_present():
        return vk
    if cpu.is_file():
        return cpu
    if vk.is_file() and backend != "cpu" and _vulkan_loader_present():
        return vk
    found = shutil.which("HCSInfer") or shutil.which("HCSInfer.exe")
    if found:
        return Path(found)
    raise ExeNotFoundError(
        "HCSInfer executable not found: this install has no bundled runtime for your platform. "
        "Set HCSINFER_EXE or put HCSInfer.exe on PATH."
    )


def run_cli(args: Sequence[str], *, exe: Optional[Path] = None, timeout: Optional[float] = None,
            state_dir: Optional[str] = None) -> "subprocess.CompletedProcess[str]":
    """Run ``HCSInfer <args>`` and capture UTF-8 text output (does not raise on a non-zero exit code)."""
    cmd: List[str] = [str(exe or find_exe())]
    if state_dir:
        cmd += ["--state-dir", state_dir]
    cmd += list(args)
    return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=timeout, check=False)


def version(exe: Optional[Path] = None) -> str:
    cp = run_cli(["--version"], exe=exe, timeout=30)
    return (cp.stdout or cp.stderr).strip()


def doctor(exe: Optional[Path] = None, timeout: float = 120.0) -> Dict[str, Any]:
    """Return the ``doctor --json`` hardware/runtime report as a dict (unknown values stay the string ``unknown``)."""
    cp = run_cli(["doctor", "--json"], exe=exe, timeout=timeout)
    if cp.returncode != 0:
        raise RuntimeError(f"doctor exited {cp.returncode}: {cp.stderr.strip()[:500]}")
    data = json.loads(cp.stdout)
    if not isinstance(data, dict):
        raise RuntimeError("doctor --json did not return an object")
    return data


def exec_exe(args: Sequence[str]) -> int:
    """Run the executable with inherited stdio and return its exit code (used by the console script)."""
    exe = find_exe()
    try:
        return subprocess.call([str(exe), *args])
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":  # pragma: no cover
    sys.exit(exec_exe(sys.argv[1:]))
