#!/usr/bin/env python3
"""FLUX.3 Klein step sweep: wall time and image difference against the 16-step render.

Runs the HCSInfer CLI (never raw sd-cli) once per step count with a fixed model, prompt, seed and size,
then compares each output with the 16-step render using ImageMagick `compare` (PSNR in dB, higher is closer,
and SSIM, 1.0 is identical). LPIPS needs a neural network and a Python ML stack that this machine does not
have in a reviewed form, so the documented metric here is PSNR + SSIM; see README.md.

Usage (all paths are arguments, nothing is hard-coded):
  python sweep.py --exe <HCSInfer.exe> --model <flux gguf> --out <dir> [--backend vulkan|cpu]
                  [--llm <file> --vae <file>] [--params-backend cpu|disk] [--steps 4,6,8,12,16]
                  [--width 512 --height 512 --seed 42 --repeat 1]

Exit code 0 only when every run succeeded; failed runs are recorded with their stderr tail, never skipped.
"""
import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path

PROMPT = "a red fox sitting in a snowy forest at dawn, detailed fur, soft light"


def run(cmd, timeout):
    t0 = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p, (time.perf_counter() - t0) * 1000.0


def compare(metric, a, b):
    # ImageMagick writes the metric to stderr; exit code 1 means "images differ", which is expected.
    p = subprocess.run(["magick", "compare", "-metric", metric, str(a), str(b), "null:"],
                       capture_output=True, text=True, timeout=120)
    txt = (p.stderr or p.stdout).strip()
    m = re.match(r"^(inf|[0-9.eE+-]+)", txt)
    if not m:
        return None
    return float("inf") if m.group(1) == "inf" else float(m.group(1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--backend", default="vulkan")
    ap.add_argument("--llm")
    ap.add_argument("--vae")
    ap.add_argument("--params-backend", dest="params_backend")
    ap.add_argument("--steps", default="4,6,8,12,16")
    ap.add_argument("--width", type=int, default=512)
    ap.add_argument("--height", type=int, default=512)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--timeout", type=int, default=3600)
    a = ap.parse_args()

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    steps = [int(x) for x in a.steps.split(",")]
    if 16 not in steps:
        steps.append(16)
    rows = []
    for n in sorted(steps, reverse=True):
        for rep in range(a.repeat):
            png = out / f"steps{n:02d}_r{rep}.png"
            cmd = [a.exe, "image", "generate", "--model", a.model, "--prompt", PROMPT,
                   "--width", str(a.width), "--height", str(a.height), "--steps", str(n),
                   "--cfg", "1.0", "--seed", str(a.seed), "--backend", a.backend,
                   "--output", str(png), "--force", "--json"]
            if a.llm:
                cmd += ["--llm", a.llm]
            if a.vae:
                cmd += ["--vae", a.vae]
            if a.params_backend:
                cmd += ["--params-backend", a.params_backend]
            p, wall = run(cmd, a.timeout)
            row = {"steps": n, "repeat": rep, "exit_code": p.returncode, "wall_ms": round(wall, 1),
                   "png": png.name}
            if p.returncode == 0:
                try:
                    j = json.loads(p.stdout)
                    row.update({k: j[k] for k in ("generation_ms", "load_ms", "backend", "device") if k in j})
                except json.JSONDecodeError:
                    row["note"] = "stdout was not JSON"
            else:
                row["stderr_tail"] = p.stderr[-600:]
            rows.append(row)
            print(json.dumps(row), flush=True)

    ref = out / "steps16_r0.png"
    for r in rows:
        if r["exit_code"] != 0 or not ref.exists():
            continue
        png = out / r["png"]
        r["psnr_db_vs_16"] = compare("PSNR", png, ref)
        r["ssim_vs_16"] = compare("SSIM", png, ref)
    (out / "sweep.json").write_text(json.dumps(
        {"prompt_chars": len(PROMPT), "width": a.width, "height": a.height, "seed": a.seed,
         "backend": a.backend, "metric": "ImageMagick PSNR/SSIM vs the 16-step render", "runs": rows}, indent=2))
    return 0 if all(r["exit_code"] == 0 for r in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
