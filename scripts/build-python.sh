#!/usr/bin/env bash
# Build the hcsinfer Python wheel from EXISTING HCSInfer binaries, in a temporary copy of python/.
#   scripts/build-python.sh --cpu <HCSInfer> [--vulkan <HCSInfer-vulkan>] [--plat <HCS_WHEEL_PLAT>] [--python python3] [--out dir]
set -euo pipefail
cpu=""; vk=""; plat=""; py="python3"; here="$(cd "$(dirname "$0")/.." && pwd)"; out="$here/artifacts/python"
while [ $# -gt 0 ]; do
  case "$1" in
    --cpu) cpu="$2"; shift 2;;
    --vulkan) vk="$2"; shift 2;;
    --plat) plat="$2"; shift 2;;
    --python) py="$2"; shift 2;;
    --out) out="$2"; shift 2;;
    *) echo "unknown argument: $1" >&2; exit 2;;
  esac
done
[ -f "$cpu" ] || { echo "CPU binary missing: $cpu" >&2; exit 2; }
[ -z "$vk" ] || [ -f "$vk" ] || { echo "Vulkan binary missing: $vk" >&2; exit 2; }
tmp="$(mktemp -d "${TMPDIR:-/tmp}/hcs-pywheel-XXXXXX")"
trap 'rm -rf -- "$tmp"' EXIT
mkdir -p "$out"
cp -R "$here/python" "$tmp/python"
mkdir -p "$tmp/python/src/hcsinfer/bin"
cp "$cpu" "$tmp/python/src/hcsinfer/bin/HCSInfer"
[ -z "$vk" ] || cp "$vk" "$tmp/python/src/hcsinfer/bin/HCSInfer-vulkan"
chmod +x "$tmp"/python/src/hcsinfer/bin/*
if [ -n "$plat" ]; then export HCS_WHEEL_PLAT="$plat"; else unset HCS_WHEEL_PLAT || true; fi
"$py" -m build --wheel "$tmp/python" --outdir "$out"
"$py" -m twine check --strict "$out"/*.whl
ls -l "$out"
