#!/usr/bin/env python3
"""Build an equivalence report from a run directory.

Input: <dir>/runs.jsonl, one JSON object per run with at least
  {"name", "backend", "params", "rep", "rc", "wall_s", "sha256"}  and <dir>/<name>.png.
Output: <out>.json and <out>.md containing
  * per-configuration repeat reproducibility (distinct hashes, worst pairwise PSNR/SSIM/max diff)
  * cross-configuration table: first successful run of each configuration vs a reference configuration
    (and optionally every pair).

Honesty rules: failed runs (rc != 0 or missing PNG) are listed, never silently dropped; infinite PSNR
is written as "inf"; LPIPS is reported as NOT RUN.

Usage: report.py <rundir> --out <prefix> [--ref backend:params] [--allpairs]
"""
from __future__ import annotations

import argparse
import itertools
import json
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import compare_files  # noqa: E402


def _fmt(v: float) -> str:
    return "inf" if v == float("inf") else f"{v:.2f}"


def load_runs(rundir: Path) -> list[dict]:
    runs = []
    for line in (rundir / "runs.jsonl").read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line:
            runs.append(json.loads(line))
    return runs


def build(rundir: Path, ref: str | None, allpairs: bool) -> dict:
    runs = load_runs(rundir)
    ok, failed = [], []
    for r in runs:
        png = rundir / f"{r['name']}.png"
        (ok if r.get("rc") == 0 and png.exists() else failed).append(r)
    groups: dict[str, list[dict]] = defaultdict(list)
    for r in ok:
        groups[f"{r['backend']}:{r['params']}"].append(r)
    for g in groups.values():
        g.sort(key=lambda r: r["rep"])

    repeat = {}
    for cfg, rs in groups.items():
        hashes = [r["sha256"] for r in rs]
        worst = {"psnr_db": float("inf"), "ssim_luma": 1.0, "max_abs_diff": 0, "frac_pixels_differing": 0.0}
        for a, b in itertools.combinations(rs, 2):
            c = compare_files(rundir / f"{a['name']}.png", rundir / f"{b['name']}.png")
            worst["psnr_db"] = min(worst["psnr_db"], c["psnr_db"])
            worst["ssim_luma"] = min(worst["ssim_luma"], c["ssim_luma"])
            worst["max_abs_diff"] = max(worst["max_abs_diff"], c["max_abs_diff"])
            worst["frac_pixels_differing"] = max(worst["frac_pixels_differing"], c["frac_pixels_differing"])
        repeat[cfg] = {
            "runs": len(rs),
            "distinct_hashes": len(set(hashes)),
            "wall_s": [r["wall_s"] for r in rs],
            "worst_pair": worst,
        }

    cfgs = sorted(groups)
    firsts = {c: groups[c][0] for c in cfgs}
    refcfg = ref if ref in groups else (cfgs[0] if cfgs else None)
    pairs = []
    if refcfg:
        todo = list(itertools.combinations(cfgs, 2)) if allpairs else [(refcfg, c) for c in cfgs if c != refcfg]
        for ca, cb in todo:
            a, b = firsts[ca], firsts[cb]
            c = compare_files(rundir / f"{a['name']}.png", rundir / f"{b['name']}.png")
            pairs.append({"a": ca, "b": cb, "a_run": a["name"], "b_run": b["name"], **c})
    return {
        "reference": refcfg,
        "repeat": repeat,
        "pairs": pairs,
        "failed_runs": [{k: r.get(k) for k in ("name", "rc", "wall_s")} for r in failed],
        "lpips": "NOT RUN (needs torch + weights; not installed)",
    }


def to_markdown(rep: dict) -> str:
    out = ["## Repeat reproducibility (same configuration, same seed)", "",
           "| config | runs | distinct sha256 | worst PSNR dB | worst SSIM | max abs diff | max frac px differing | wall s |",
           "|---|---|---|---|---|---|---|---|"]
    for cfg, r in sorted(rep["repeat"].items()):
        w = r["worst_pair"]
        out.append(f"| `{cfg}` | {r['runs']} | {r['distinct_hashes']} | {_fmt(w['psnr_db'])} | {w['ssim_luma']:.5f} | "
                   f"{w['max_abs_diff']} | {w['frac_pixels_differing']:.4f} | {', '.join(str(x) for x in r['wall_s'])} |")
    out += ["", f"## Cross-configuration (first run of each; reference `{rep['reference']}`)", "",
            "| A | B | byte-identical | pixel-identical | PSNR dB | SSIM | max abs diff | mean abs diff | px differing | px diff>8 |",
            "|---|---|---|---|---|---|---|---|---|---|"]
    for p in rep["pairs"]:
        out.append(f"| `{p['a']}` | `{p['b']}` | {p['byte_identical']} | {p['pixel_identical']} | {_fmt(p['psnr_db'])} | "
                   f"{p['ssim_luma']:.5f} | {p['max_abs_diff']} | {p['mean_abs_diff']:.3f} | "
                   f"{p['frac_pixels_differing']:.4f} | {p['frac_pixels_diff_gt8']:.4f} |")
    if rep["failed_runs"]:
        out += ["", "## Failed runs", ""] + [f"- {f['name']}: rc={f['rc']}" for f in rep["failed_runs"]]
    out += ["", f"LPIPS: {rep['lpips']}", ""]
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("rundir")
    ap.add_argument("--out", required=True, help="output prefix (writes .json and .md)")
    ap.add_argument("--ref", default=None)
    ap.add_argument("--allpairs", action="store_true")
    a = ap.parse_args(argv)
    rep = build(Path(a.rundir), a.ref, a.allpairs)
    Path(a.out + ".json").write_text(
        json.dumps(rep, indent=2, default=lambda o: "inf" if o == float("inf") else o), encoding="utf-8")
    Path(a.out + ".md").write_text(to_markdown(rep), encoding="utf-8")
    print(to_markdown(rep))
    return 0


if __name__ == "__main__":
    sys.exit(main())
