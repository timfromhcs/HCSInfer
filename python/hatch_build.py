"""Hatch build hook: tag the wheel for the platform of the bundled HCSInfer binary(ies).

Staging directory ``src/hcsinfer/bin/`` may hold ``HCSInfer(.exe)`` (CPU) and ``HCSInfer-vulkan(.exe)``.

* Binary present: wheel is ``py3-none-<plat>`` (not pure). ``<plat>`` comes from the environment variable
  ``HCS_WHEEL_PLAT`` (validated against an allow-list) or, when unset, is derived from the build host.
* No binary: pure ``py3-none-any`` client + launcher wheel (needs ``HCSINFER_EXE`` or a binary on PATH).
"""
from __future__ import annotations

import os
import platform
import sys
from typing import Any, Dict, Optional

from hatchling.builders.hooks.plugin.interface import BuildHookInterface

ALLOWED_PLATS = (
    "win_amd64",
    "win_arm64",
    "manylinux_2_28_x86_64",
    "manylinux_2_28_aarch64",
    "macosx_11_0_arm64",
    "macosx_10_15_x86_64",
)
BIN_NAMES = ("HCSInfer", "HCSInfer.exe", "HCSInfer-vulkan", "HCSInfer-vulkan.exe")


def validate_plat(plat: str) -> str:
    """Return ``plat`` if it is in the allow-list, else raise ``ValueError`` (no free-form tags)."""
    if plat not in ALLOWED_PLATS:
        raise ValueError(f"HCS_WHEEL_PLAT={plat!r} is not allowed; choose one of: {', '.join(ALLOWED_PLATS)}")
    return plat


def host_plat(system: Optional[str] = None, machine: Optional[str] = None) -> str:
    """Derive the wheel platform tag from the build host (arguments exist for testing)."""
    system = system if system is not None else platform.system()
    machine = (machine if machine is not None else platform.machine()).lower()
    table: Dict[str, Dict[str, str]] = {
        "Windows": {"amd64": "win_amd64", "x86_64": "win_amd64", "arm64": "win_arm64"},
        "Linux": {"x86_64": "manylinux_2_28_x86_64", "amd64": "manylinux_2_28_x86_64",
                  "aarch64": "manylinux_2_28_aarch64", "arm64": "manylinux_2_28_aarch64"},
        "Darwin": {"arm64": "macosx_11_0_arm64", "x86_64": "macosx_10_15_x86_64"},
    }
    plat = table.get(system, {}).get(machine)
    if plat is None:
        raise ValueError(f"cannot derive a wheel platform tag for {system}/{machine}; set HCS_WHEEL_PLAT")
    return plat


def select_tag(has_binary: bool, env: Optional[Dict[str, str]] = None,
               system: Optional[str] = None, machine: Optional[str] = None) -> str:
    """Return the full wheel tag (``py3-none-any`` without a binary)."""
    if not has_binary:
        return "py3-none-any"
    env = os.environ if env is None else env
    explicit = env.get("HCS_WHEEL_PLAT", "").strip()
    plat = validate_plat(explicit) if explicit else host_plat(system, machine)
    return f"py3-none-{plat}"


class CustomBuildHook(BuildHookInterface):  # type: ignore[misc]
    def initialize(self, version: str, build_data: Dict[str, Any]) -> None:
        if self.target_name != "wheel":
            return
        bindir = os.path.join(self.root, "src", "hcsinfer", "bin")
        has_binary = any(os.path.isfile(os.path.join(bindir, n)) for n in BIN_NAMES)
        if not has_binary:
            if os.environ.get("HCS_WHEEL_PLAT", "").strip():
                print("hatch_build: HCS_WHEEL_PLAT set but no binary staged; building pure py3-none-any",
                      file=sys.stderr)
            return
        build_data["pure_python"] = False
        build_data["infer_tag"] = False
        build_data["tag"] = select_tag(True)
