"""hcsinfer: Python launcher and client for the HCSInfer local AI runtime."""
from __future__ import annotations

from ._exe import ExeNotFoundError, doctor, find_exe, run_cli, version
from .client import Client, HcsError
from .server import Server

__version__ = "0.1.0"
__all__ = [
    "Client",
    "ExeNotFoundError",
    "HcsError",
    "Server",
    "__version__",
    "doctor",
    "find_exe",
    "run_cli",
    "version",
]
