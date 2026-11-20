#!/usr/bin/env python3
"""Compare two summary.json files against regression-thresholds.json (AGENTS.md 16.6).

Usage: check_regression.py <baseline summary.json> <candidate summary.json> [--thresholds tools/bench/regression-thresholds.json]
Exit code 0: no alert, 1: at least one metric regressed beyond its threshold, 2: usage/IO error.
Metrics present in only one file are listed as 'unmatched' and never alert.
"""
import argparse, json, os, sys


def rule_for(key, rules, default):
    best = None
    for r in rules:
        if not key.startswith(r["match"]):
            continue
        if "suffix" in r and not key.endswith(r["suffix"]):
            continue
        if "contains" in r and r["contains"] not in key:
            continue
        best = r
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("baseline")
    ap.add_argument("candidate")
    ap.add_argument("--thresholds", default=os.path.join(os.path.dirname(__file__), "regression-thresholds.json"))
    a = ap.parse_args()
    try:
        base = json.load(open(a.baseline, encoding="utf-8"))["metrics"]
        cand = json.load(open(a.candidate, encoding="utf-8"))["metrics"]
        th = json.load(open(a.thresholds, encoding="utf-8"))
    except Exception as e:
        print("error:", e, file=sys.stderr)
        return 2
    alerts, ok, unmatched = [], 0, []
    for k, b in sorted(base.items()):
        if k not in cand:
            unmatched.append(k)
            continue
        r = rule_for(k, th["rules"], th["default_pct"])
        if not r or not b:
            unmatched.append(k)
            continue
        c = cand[k]
        change = (c - b) / b * 100.0
        worse = -change if r["direction"] == "higher_is_better" else change
        if worse > r["pct"]:
            alerts.append((k, b, c, change, r["pct"]))
        else:
            ok += 1
    for k, b, c, ch, pct in alerts:
        print(f"ALERT {k}: {b:.3f} -> {c:.3f} ({ch:+.1f}%, limit {pct}%)")
    print(f"{ok} within threshold, {len(alerts)} alerts, {len(unmatched)} unmatched")
    return 1 if alerts else 0


if __name__ == "__main__":
    sys.exit(main())
