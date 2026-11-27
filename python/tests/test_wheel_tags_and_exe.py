"""Wheel tag selection/validation (hatch_build.py) and bundled-binary discovery (hcsinfer._exe), no real runtime."""
from __future__ import annotations

import importlib.util
import os
import sys
from pathlib import Path
from types import ModuleType
from typing import Any, Dict

import pytest

from hcsinfer import ExeNotFoundError, find_exe
from hcsinfer import _exe

pytest.importorskip("hatchling")
_HOOK = Path(__file__).resolve().parents[1] / "hatch_build.py"


def _load_hook() -> ModuleType:
    spec = importlib.util.spec_from_file_location("hcs_hatch_build", _HOOK)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


hook = _load_hook()


# ---- tag selection -------------------------------------------------------------------------------------------

def test_no_binary_is_pure_any_even_with_env() -> None:
    assert hook.select_tag(False, {"HCS_WHEEL_PLAT": "win_amd64"}) == "py3-none-any"


@pytest.mark.parametrize("plat", list(hook.ALLOWED_PLATS))
def test_explicit_plat_allowed(plat: str) -> None:
    assert hook.select_tag(True, {"HCS_WHEEL_PLAT": plat}) == f"py3-none-{plat}"


@pytest.mark.parametrize("bad", ["linux_x86_64", "any", "win32", "manylinux_2_28_x86_64; rm -rf", "../x", "WIN_AMD64"])
def test_explicit_plat_rejected(bad: str) -> None:
    with pytest.raises(ValueError, match="not allowed"):
        hook.select_tag(True, {"HCS_WHEEL_PLAT": bad})


@pytest.mark.parametrize(
    ("system", "machine", "expected"),
    [
        ("Windows", "AMD64", "win_amd64"),
        ("Windows", "ARM64", "win_arm64"),
        ("Linux", "x86_64", "manylinux_2_28_x86_64"),
        ("Linux", "aarch64", "manylinux_2_28_aarch64"),
        ("Darwin", "arm64", "macosx_11_0_arm64"),
        ("Darwin", "x86_64", "macosx_10_15_x86_64"),
    ],
)
def test_host_derivation(system: str, machine: str, expected: str) -> None:
    assert hook.select_tag(True, {}, system, machine) == f"py3-none-{expected}"
    assert expected in hook.ALLOWED_PLATS


def test_host_derivation_unknown_host() -> None:
    with pytest.raises(ValueError, match="HCS_WHEEL_PLAT"):
        hook.select_tag(True, {}, "FreeBSD", "amd64")


def test_blank_env_falls_back_to_host() -> None:
    assert hook.select_tag(True, {"HCS_WHEEL_PLAT": "  "}, "Linux", "x86_64") == "py3-none-manylinux_2_28_x86_64"


_Base: Any = hook.CustomBuildHook


class _Hook(_Base):  # type: ignore[misc]
    def __init__(self, root: Path, target: str = "wheel") -> None:  # noqa: D107  (skip hatchling's heavy ctor)
        self._root = str(root)
        self._target = target

    @property
    def root(self) -> str:
        return self._root

    @property
    def target_name(self) -> str:
        return self._target


def _stage(root: Path, *names: str) -> None:
    b = root / "src" / "hcsinfer" / "bin"
    b.mkdir(parents=True)
    for n in names:
        (b / n).write_bytes(b"x")


def test_hook_pure_without_binary(tmp_path: Path) -> None:
    data: Dict[str, Any] = {}
    _Hook(tmp_path).initialize("1", data)
    assert data == {}


@pytest.mark.parametrize("name", ["HCSInfer", "HCSInfer.exe", "HCSInfer-vulkan", "HCSInfer-vulkan.exe"])
def test_hook_tags_with_any_binary(tmp_path: Path, monkeypatch: pytest.MonkeyPatch, name: str) -> None:
    monkeypatch.setenv("HCS_WHEEL_PLAT", "manylinux_2_28_aarch64")
    _stage(tmp_path, name)
    data: Dict[str, Any] = {}
    _Hook(tmp_path).initialize("1", data)
    assert data == {"pure_python": False, "infer_tag": False, "tag": "py3-none-manylinux_2_28_aarch64"}


def test_hook_ignores_sdist(tmp_path: Path) -> None:
    _stage(tmp_path, "HCSInfer")
    data: Dict[str, Any] = {}
    _Hook(tmp_path, "sdist").initialize("1", data)
    assert data == {}


# ---- binary discovery ----------------------------------------------------------------------------------------

@pytest.fixture()
def clean_env(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> Path:
    monkeypatch.delenv("HCSINFER_EXE", raising=False)
    monkeypatch.delenv("HCSINFER_BACKEND", raising=False)
    monkeypatch.setenv("PATH", str(tmp_path / "empty"))  # nothing on PATH
    b = tmp_path / "bin"
    b.mkdir()
    monkeypatch.setattr(_exe, "_BIN", b)
    return b


@pytest.mark.parametrize(("platform", "sfx"), [("win32", ".exe"), ("linux", ""), ("darwin", "")])
def test_find_cpu_binary_per_os(clean_env: Path, monkeypatch: pytest.MonkeyPatch, platform: str, sfx: str) -> None:
    monkeypatch.setattr(sys, "platform", platform)
    (clean_env / f"HCSInfer{sfx}").write_bytes(b"x")
    assert find_exe().name == f"HCSInfer{sfx}"


@pytest.mark.parametrize(("platform", "sfx"), [("win32", ".exe"), ("linux", "")])
def test_vulkan_chosen_only_with_loader(clean_env: Path, monkeypatch: pytest.MonkeyPatch, platform: str, sfx: str) -> None:
    monkeypatch.setattr(sys, "platform", platform)
    (clean_env / f"HCSInfer{sfx}").write_bytes(b"x")
    (clean_env / f"HCSInfer-vulkan{sfx}").write_bytes(b"x")
    monkeypatch.setattr(_exe, "_vulkan_loader_present", lambda: False)
    assert find_exe().name == f"HCSInfer{sfx}"
    monkeypatch.setattr(_exe, "_vulkan_loader_present", lambda: True)
    assert find_exe().name == f"HCSInfer-vulkan{sfx}"
    assert find_exe("cpu").name == f"HCSInfer{sfx}"
    monkeypatch.setenv("HCSINFER_BACKEND", "cpu")
    assert find_exe().name == f"HCSInfer{sfx}"


def test_vulkan_only_wheel_without_loader_not_used_for_auto(clean_env: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sys, "platform", "linux")
    (clean_env / "HCSInfer-vulkan").write_bytes(b"x")
    monkeypatch.setattr(_exe, "_vulkan_loader_present", lambda: False)
    with pytest.raises(ExeNotFoundError):
        find_exe()


def test_macos_never_has_vulkan(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sys, "platform", "darwin")
    assert _exe._vulkan_loader_present() is False


def test_windows_loader_probe(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.setenv("SystemRoot", str(tmp_path))
    assert _exe._vulkan_loader_present() is False
    (tmp_path / "System32").mkdir()
    (tmp_path / "System32" / "vulkan-1.dll").write_bytes(b"x")
    assert _exe._vulkan_loader_present() is True


def test_linux_loader_probe(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sys, "platform", "linux")
    import ctypes.util

    monkeypatch.setattr(ctypes.util, "find_library", lambda name: "libvulkan.so.1" if name == "vulkan" else None)
    assert _exe._vulkan_loader_present() is True


@pytest.mark.skipif(os.name == "nt", reason="POSIX exec bit")
def test_exec_bit_repaired(clean_env: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(sys, "platform", "linux")
    p = clean_env / "HCSInfer"
    p.write_bytes(b"x")
    p.chmod(0o644)
    assert os.access(find_exe(), os.X_OK)


def test_explicit_env_and_missing(clean_env: Path, monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    f = tmp_path / "custom"
    f.write_bytes(b"x")
    monkeypatch.setenv("HCSINFER_EXE", str(f))
    assert find_exe() == f
    monkeypatch.setenv("HCSINFER_EXE", str(tmp_path / "nope"))
    with pytest.raises(ExeNotFoundError):
        find_exe()


def test_import_does_not_run_anything() -> None:
    import subprocess

    cp = subprocess.run([sys.executable, "-c", "import hcsinfer, sys; print('ok')"], capture_output=True, text=True,
                        env={**os.environ, "HCSINFER_EXE": "/nonexistent"}, check=False)
    assert cp.returncode == 0 and cp.stdout.strip() == "ok"
