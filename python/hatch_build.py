"""Hatch build hook: tag the wheel py3-none-win_amd64 when a prebuilt HCSInfer.exe is bundled.

Without src/hcsinfer/bin/*.exe the wheel stays a pure py3-none-any wheel (so an sdist can be installed anywhere,
it simply has no bundled runtime and relies on HCSINFER_EXE / PATH).
"""
from __future__ import annotations

import glob
import os
from typing import Any

from hatchling.builders.hooks.plugin.interface import BuildHookInterface


class CustomBuildHook(BuildHookInterface):  # type: ignore[misc]
    def initialize(self, version: str, build_data: dict[str, Any]) -> None:
        if self.target_name != "wheel":
            return
        exes = glob.glob(os.path.join(self.root, "src", "hcsinfer", "bin", "*.exe"))
        if exes:
            build_data["pure_python"] = False
            build_data["infer_tag"] = False
            build_data["tag"] = "py3-none-win_amd64"
