#!/usr/bin/env python3
"""Generate summary.json, Markdown tables and SVG charts from the raw benchmark JSON (wave D).

Usage: make_report.py --data docs/test-evidence/benchmarks --charts docs/benchmarks --tables <out.md> --readme-table <out.md>
Only values present in the raw JSON are used; a missing file renders as 'NOT RUN'.
"""
from __future__ import annotations
import argparse, glob, json, os, statistics

MODELS = [("qwen3-0.6b-q4_k_m", "qwen3-0.6b Q4_K_M"), ("lfm3-omni-2b-q8_0", "LFM3-Omni-2B Q8_0")]
BACKENDS = [("cpu", "CPU (8 threads)"), ("vulkan", "Vulkan (Radeon 680M)")]
COL = {"cpu": "#5b7db1", "vulkan": "#d9822b", "f16": "#5b7db1", "q8_0": "#3f9d73", "q4_0": "#c4553f",
       "fast": "#d9822b", "quality": "#8a5bb1"}


def load(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def med_p90(xs):
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None, None
    if len(xs) == 1:
        return xs[0], xs[0]
    return statistics.median(xs), xs[int(round(0.9 * (len(xs) - 1)))]


def fmt(v, nd=1, unit=""):
    if v is None:
        return "NOT RUN"
    if isinstance(v, str):
        return v
    return f"{v:,.{nd}f}{unit}"


def derived(tokens, ttft_ms):
    # Vulkan submits asynchronously, so the engine's own prompt t/s field is not meaningful there; tokens / TTFT is used for both backends.
    return tokens / (ttft_ms / 1000) if tokens and ttft_ms else None


def mib(b):
    return None if b is None else b / (1 << 20)


# ---------------------------------------------------------------- collection
def collect(d):
    S = {"text": {}, "llamabench": {}, "kv": {}, "vision": {}, "image": {}, "api": {}, "hdd": None, "env": load(f"{d}/environment.json")}
    for m, _ in MODELS:
        for b, _ in BACKENDS:
            j = load(f"{d}/text-full-{b}-{m}.json")
            run = load(f"{d}/text-full-{b}-{m}.run.json")
            if j:
                S["text"][(m, b)] = (j, run)
            lb = load(f"{d}/llamabench-{b}-{m}.json")
            if lb:
                S["llamabench"][(m, b)] = lb
            v = load(f"{d}/vision-{b}-{m}.json")
            if v:
                S["vision"][(m, b)] = v
    for b, _ in BACKENDS:
        for ctx in (4096, 8192):
            for ty in ("f16", "q8_0", "q4_0"):
                k = load(f"{d}/kv-{b}-{ty}-{ctx}.json")
                if k:
                    S["kv"][(b, ty, ctx)] = k
    for tag in ("sd15", "flux3-fast", "flux3-quality"):
        for b, _ in BACKENDS:
            j = load(f"{d}/image-{tag}-{b}.json")
            if j:
                S["image"][(tag, b)] = j
    for b, _ in BACKENDS:
        a = load(f"{d}/api-latency-{b}-lfm3-omni-2b-q8_0.json")
        if a:
            S["api"][b] = a
    S["hdd"] = load(f"{d}/hdd-read.json")
    return S


def lb_value(lb, kind):
    """llama-bench json -> (avg t/s, stddev, samples) for pp or tg."""
    if not lb or not lb.get("results"):
        return None
    for r in lb["results"]:
        if kind == "pp" and r.get("n_prompt") and not r.get("n_gen"):
            return r
        if kind == "tg" and r.get("n_gen") and not r.get("n_prompt"):
            return r
    return None


def image_stats(j):
    ok = [r for r in j["runs"] if r["rc"] == 0 and not r["timed_out"]]
    walls = [r["wall_s"] for r in ok]
    med, p90 = med_p90(walls)
    return {"n_ok": len(ok), "n": len(j["runs"]), "median_s": med, "p90_s": p90, "walls": walls,
            "peak_ws_mib": mib(max((r["peak_ws_bytes"] or 0) for r in ok)) if ok else None,
            "peak_commit_mib": mib(max((r["peak_commit_bytes"] or 0) for r in ok)) if ok else None,
            "failures": [(r["rc"], r["timed_out"]) for r in j["runs"] if r["rc"] != 0 or r["timed_out"]]}


# ---------------------------------------------------------------- summary (regression baseline)
def summary(S):
    s = {}
    for (m, b), (j, _) in S["text"].items():
        k = f"text.{m}.{b}"
        s[f"{k}.decode_tps"] = j.get("decode_tokens_per_second_median")
        s[f"{k}.ttft_ms"] = j.get("ttft_ms_median")
        s[f"{k}.prompt_tps"] = derived(j.get("prompt_tokens"), j.get("ttft_ms_median"))
        s[f"{k}.warm_load_ms"] = j.get("load_ms_median")
        for pt in j.get("prefill_sweep", []):
            if pt.get("result_status") == "PASS":
                s[f"{k}.prefill_ttft_ms.{pt.get('target_prompt_tokens')}"] = pt.get("ttft_ms_median")
        for pt in j.get("decode_sweep", []):
            if pt.get("result_status") == "PASS":
                s[f"{k}.decode_tps.{pt.get('target_prompt_tokens')}"] = pt.get("decode_tokens_per_second_median")
    for (m, b), lb in S["llamabench"].items():
        for kind in ("pp", "tg"):
            r = lb_value(lb, kind)
            if r:
                s[f"llamabench.{m}.{b}.{kind}_tps"] = r.get("avg_ts")
    for (tag, b), j in S["image"].items():
        st = image_stats(j)
        s[f"image.{tag}.{b}.wall_s"] = st["median_s"]
    for b, a in S["api"].items():
        s[f"api.{b}.stream_ttft_ms"] = a["stream_ttft_ms"]["median"]
    s = {k: v for k, v in s.items() if v is not None}
    return s


# ---------------------------------------------------------------- tables
def tables(S):
    o = []
    o.append("### Text: `HCSInfer benchmark --suite full` (warm medians over 5 measured iterations, 1 warm-up discarded)\n")
    o.append("| Model | Backend | status | cold load | cold TTFT | warm load med / p90 | warm TTFT med / p90 | prompt tokens / TTFT (t/s) | decode t/s med / p90 | peak WS / commit |")
    o.append("|---|---|---|---|---|---|---|---|---|---|")
    for m, ml in MODELS:
        for b, bl in BACKENDS:
            t = S["text"].get((m, b))
            if not t:
                o.append(f"| {ml} | {bl} | NOT RUN | | | | | | | |")
                continue
            j, run = t
            c = j.get("cold", {})
            o.append(f"| {ml} | {bl} | {j.get('result_status')} | {fmt(c.get('load_ms_median'), 0, ' ms')} | {fmt(c.get('ttft_ms_median'), 0, ' ms')} | "
                     f"{fmt(j.get('load_ms_median'), 0)} / {fmt(j.get('load_ms_p90'), 0)} ms | {fmt(j.get('ttft_ms_median'), 0)} / {fmt(j.get('ttft_ms_p90'), 0)} ms | "
                     f"{fmt(derived(j.get('prompt_tokens'), j.get('ttft_ms_median')), 1)} | {fmt(j.get('decode_tokens_per_second_median'), 2)} / {fmt(j.get('decode_tokens_per_second_p90'), 2)} | "
                     f"{fmt(mib(j.get('peak_ram_bytes')), 0, ' MiB')} / {fmt(mib(j.get('peak_commit_bytes')), 0, ' MiB')} |")
    o.append("")
    o.append("### Prefill sweep: TTFT and prompt throughput (1 new token; median / p90 of 5)\n")
    o.append("| Model | Backend | prompt tokens | TTFT ms (med / p90) | prompt tokens / TTFT (t/s) | decode t/s at that prompt size (med) |")
    o.append("|---|---|---|---|---|---|")
    for m, ml in MODELS:
        for b, bl in BACKENDS:
            t = S["text"].get((m, b))
            if not t:
                continue
            j, _ = t
            dec = {p.get("target_prompt_tokens"): p for p in j.get("decode_sweep", [])}
            for pt in j.get("prefill_sweep", []):
                tp = pt.get("target_prompt_tokens")
                d = dec.get(tp, {})
                if pt.get("result_status") != "PASS":
                    o.append(f"| {ml} | {bl} | {pt.get('prompt_tokens', tp)} | {pt.get('result_status')} | | |")
                    continue
                o.append(f"| {ml} | {bl} | {pt.get('prompt_tokens')} | {fmt(pt.get('ttft_ms_median'), 0)} / {fmt(pt.get('ttft_ms_p90'), 0)} | "
                         f"{fmt(derived(pt.get('prompt_tokens'), pt.get('ttft_ms_median')), 1)} | {fmt(d.get('decode_tokens_per_second_median'), 2)} |")
    o.append("")
    o.append("### Cross-check with upstream `llama-bench` (pp512 / tg128, 5 repetitions; same pinned llama.cpp)\n")
    o.append("| Model | Backend | pp512 t/s (mean ± sd) | tg128 t/s (mean ± sd) | peak WS / commit |")
    o.append("|---|---|---|---|---|")
    for m, ml in MODELS:
        for b, bl in BACKENDS:
            lb = S["llamabench"].get((m, b))
            if not lb:
                o.append(f"| {ml} | {bl} | NOT RUN | | |")
                continue
            pp, tg = lb_value(lb, "pp"), lb_value(lb, "tg")
            f2 = lambda r: "n/a" if not r else f"{r['avg_ts']:,.1f} ± {r.get('stddev_ts', 0):,.1f}"
            o.append(f"| {ml} | {bl} | {f2(pp)} | {f2(tg)} | {fmt(mib(lb.get('peak_ws_bytes')), 0, ' MiB')} / {fmt(mib(lb.get('peak_commit_bytes')), 0, ' MiB')} |")
    o.append("")
    o.append("### Vision: LFM3-Omni-2B Q8_0 + mmproj, 512x512 test image, 48 max new tokens (separate process per run)\n")
    o.append("| Backend | n ok / n | load ms (med / p90) | TTFT incl. image encode ms (med / p90) | decode t/s (med) | wall s (med) | peak WS / commit |")
    o.append("|---|---|---|---|---|---|---|")
    for b, bl in BACKENDS:
        v = S["vision"].get(("lfm3-omni-2b-q8_0", b))
        if not v:
            o.append(f"| {bl} | NOT RUN | | | | | |")
            continue
        ok = [r for r in v["runs"] if r["rc"] == 0 and r.get("metrics")]
        g = lambda k: [r["metrics"].get(k) for r in ok]
        lm, l9 = med_p90(g("load_ms"))
        tm, t9 = med_p90(g("ttft_ms"))
        dm, _ = med_p90(g("decode_tokens_per_second"))
        wm, _ = med_p90([r["wall_s"] for r in ok])
        o.append(f"| {bl} | {len(ok)} / {len(v['runs'])} | {fmt(lm, 0)} / {fmt(l9, 0)} | {fmt(tm, 0)} / {fmt(t9, 0)} | {fmt(dm, 1)} | {fmt(wm, 1)} | "
                 f"{fmt(mib(max((r['peak_ws_bytes'] or 0) for r in ok)) if ok else None, 0, ' MiB')} / {fmt(mib(max((r['peak_commit_bytes'] or 0) for r in ok)) if ok else None, 0, ' MiB')} |")
    o.append("")
    o.append("### Image generation, 512x512, seed 42, wall time of a whole `image generate` process (load + sampling + VAE + PNG)\n")
    o.append("| Model | Setting | Backend | n ok / n | wall s median (p90) | all samples (s) | peak WS / commit |")
    o.append("|---|---|---|---|---|---|---|")
    for tag, lab in (("sd15", "SD1.5 Q8_0, 20 steps"), ("flux3-fast", "FLUX.3 Klein UD-Q4_K_XL, preset fast (6 steps)"),
                     ("flux3-quality", "FLUX.3 Klein UD-Q4_K_XL, preset quality (16 steps)")):
        for b, bl in BACKENDS:
            j = S["image"].get((tag, b))
            mdl, setting = (lab.split(", ", 1) + [""])[:2] if tag == "sd15" else (lab.split(", ", 1)[0], lab.split(", ", 1)[1])
            if not j:
                o.append(f"| {mdl} | {setting} | {bl} | NOT RUN | | | |")
                continue
            st = image_stats(j)
            fails = f" (failures: {st['failures']})" if st["failures"] else ""
            o.append(f"| {mdl} | {setting} | {bl} | {st['n_ok']} / {st['n']}{fails} | {fmt(st['median_s'], 1)} ({fmt(st['p90_s'], 1)}) | "
                     f"{', '.join(f'{w:.1f}' for w in st['walls']) or '-'} | {fmt(st['peak_ws_mib'], 0, ' MiB')} / {fmt(st['peak_commit_mib'], 0, ' MiB')} |")
    o.append("")
    o.append("### KV cache type x context (upstream `llama-bench`, qwen3-0.6b Q4_K_M, flash attention on, K and V same type, pp = whole context; repetitions per row in the status column)\n")
    o.append("| Backend | context | cache | pp t/s (mean ± sd) | TTFT proxy s (= ctx / pp) | peak WS MiB | peak commit MiB | status |")
    o.append("|---|---|---|---|---|---|---|---|")
    for b, bl in BACKENDS:
        for ctx in (4096, 8192):
            for ty in ("f16", "q8_0", "q4_0"):
                k = S["kv"].get((b, ty, ctx))
                if not k:
                    o.append(f"| {bl} | {ctx} | {ty} | NOT RUN | | | | |")
                    continue
                r = lb_value(k, "pp")
                if k["rc"] != 0 or not r:
                    o.append(f"| {bl} | {ctx} | {ty} | | | | | FAILED rc={k['rc']}: {k['stderr_tail'].strip().splitlines()[-1][:70] if k['stderr_tail'].strip() else ''} |")
                    continue
                o.append(f"| {bl} | {ctx} | {ty} | {r['avg_ts']:,.1f} ± {r.get('stddev_ts', 0):,.1f} | {ctx / r['avg_ts']:.2f} | "
                         f"{fmt(mib(k['peak_ws_bytes']), 0)} | {fmt(mib(k['peak_commit_bytes']), 0)} | PASS, n={k['reps']} |")
    o.append("")
    o.append("### HTTP API latency (`HCSInfer serve`, loopback, LFM3-Omni-2B Q8_0, 10 measured requests after 1 discarded)\n")
    o.append("| Backend | first request incl. model load | stream TTFT ms (med / p90) | stream total ms (med) | non-stream total ms (med / p90) | server peak WS / commit |")
    o.append("|---|---|---|---|---|---|")
    for b, bl in BACKENDS:
        a = S["api"].get(b)
        if not a:
            o.append(f"| {bl} | NOT RUN | | | | |")
            continue
        o.append(f"| {bl} | {fmt(a['first_request_incl_model_load_ms'], 0, ' ms')} | {fmt(a['stream_ttft_ms']['median'], 0)} / {fmt(a['stream_ttft_ms']['p90'], 0)} | "
                 f"{fmt(a['stream_total_ms']['median'], 0)} | {fmt(a['nonstream_total_ms']['median'], 0)} / {fmt(a['nonstream_total_ms']['p90'], 0)} | "
                 f"{fmt(mib(a['server_peak_ws_bytes']), 0, ' MiB')} / {fmt(mib(a['server_peak_commit_bytes']), 0, ' MiB')} |")
    o.append("")
    h = S["hdd"]
    o.append("### Model drive: sequential read, read-only (`HCSInfer doctor --read-file`, 4 MiB buffer, up to 1 GiB per sample)\n")
    if h:
        o.append("| File | phase | MiB/s (median) | samples |")
        o.append("|---|---|---|---|")
        for r in h["results"]:
            rb = r.get("read_benchmark") or {}
            o.append(f"| `{r['file']}` | {r['phase']} | {fmt(rb.get('mib_s_median'), 1)} | {len(rb.get('samples', []))} |")
    else:
        o.append("NOT RUN")
    o.append("")
    return "\n".join(o)


def readme_table(S):
    def t(m, b, key, nd=1):
        x = S["text"].get((m, b))
        return fmt(x[0].get(key), nd) if x else "NOT RUN"
    o = ["| Workload (this PC, Ryzen 7 7735HS + Radeon 680M) | CPU | Vulkan |", "|---|---|---|"]
    for m, ml in MODELS:
        o.append(f"| {ml}: decode tokens/s (median) | {t(m, 'cpu', 'decode_tokens_per_second_median', 1)} | {t(m, 'vulkan', 'decode_tokens_per_second_median', 1)} |")
        o.append(f"| {ml}: warm TTFT ms (median, ~200-225-token prompt) | {t(m, 'cpu', 'ttft_ms_median', 0)} | {t(m, 'vulkan', 'ttft_ms_median', 0)} |")
    for tag, lab in (("sd15", "SD1.5 Q8_0 512x512, 20 steps (wall s)"), ("flux3-fast", "FLUX.3 Klein 512x512, `fast` preset (wall s)"),
                     ("flux3-quality", "FLUX.3 Klein 512x512, `quality` preset (wall s)")):
        cells = []
        for b, _ in BACKENDS:
            j = S["image"].get((tag, b))
            st = image_stats(j) if j else None
            cells.append(fmt(st["median_s"], 0) + (f" (n={st['n_ok']})" if st["n_ok"] != 5 else "") if st and st["median_s"] is not None else "NOT RUN")
        o.append(f"| {lab} | {cells[0]} | {cells[1]} |")
    v = []
    for b, _ in BACKENDS:
        x = S["vision"].get(("lfm3-omni-2b-q8_0", b))
        ok = [r for r in x["runs"] if r["rc"] == 0 and r.get("metrics")] if x else []
        v.append(fmt(med_p90([r["metrics"]["ttft_ms"] for r in ok])[0], 0) if ok else "NOT RUN")
    o.append(f"| Vision (LFM3-Omni + mmproj, 512x512 image): TTFT ms | {v[0]} | {v[1]} |")
    return "\n".join(o) + "\n"


# ---------------------------------------------------------------- SVG
def esc(s):
    return str(s).replace("&", "&amp;").replace("<", "&lt;")


def svg_wrap(w, h, title, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" role="img" aria-label="{esc(title)}" '
            f'font-family="Segoe UI, system-ui, sans-serif" font-size="12">\n<rect width="{w}" height="{h}" rx="8" fill="#ffffff" stroke="#d0d7de"/>\n'
            f'<text x="16" y="26" font-size="15" font-weight="600" fill="#1f2328">{esc(title)}</text>\n{body}</svg>\n')


def grouped_bars(title, groups, series, unit, w=720, h=340, nd=1, note=""):
    """groups: list of labels; series: list of (name, colour, [values or None])."""
    left, right, top, bot = 60, 20, 56, 70
    pw, ph = w - left - right, h - top - bot
    vmax = max([v for _, _, vs in series for v in vs if v is not None] + [1e-9]) * 1.12
    out = []
    for i in range(5):
        y = top + ph - ph * i / 4
        out.append(f'<line x1="{left}" y1="{y:.1f}" x2="{w - right}" y2="{y:.1f}" stroke="#eaeef2"/>'
                   f'<text x="{left - 6}" y="{y + 4:.1f}" text-anchor="end" fill="#57606a">{vmax * i / 4:,.0f}</text>')
    gw = pw / max(1, len(groups))
    bw = gw * 0.7 / max(1, len(series))
    for gi, g in enumerate(groups):
        gx = left + gi * gw + gw * 0.15
        for si, (name, colr, vs) in enumerate(series):
            v = vs[gi]
            x = gx + si * bw
            if v is None:
                out.append(f'<text x="{x + bw / 2:.1f}" y="{top + ph - 6}" text-anchor="middle" fill="#8c959f" font-size="10">n/a</text>')
                continue
            bh = ph * v / vmax
            out.append(f'<rect x="{x:.1f}" y="{top + ph - bh:.1f}" width="{bw - 3:.1f}" height="{bh:.1f}" fill="{colr}" rx="2"/>'
                       f'<text x="{x + bw / 2 - 1.5:.1f}" y="{top + ph - bh - 4:.1f}" text-anchor="middle" fill="#1f2328" font-size="11">{v:,.{nd}f}</text>')
        out.append(f'<text x="{left + gi * gw + gw / 2:.1f}" y="{h - bot + 18}" text-anchor="middle" fill="#1f2328">{esc(g)}</text>')
    lx = left
    for name, colr, _ in series:
        out.append(f'<rect x="{lx}" y="{h - 30}" width="12" height="12" fill="{colr}" rx="2"/><text x="{lx + 18}" y="{h - 20}" fill="#1f2328">{esc(name)}</text>')
        lx += 24 + 7 * len(name)
    out.append(f'<text x="{w - right}" y="{h - 20}" text-anchor="end" fill="#57606a" font-size="11">{esc(unit)}{(" - " + esc(note)) if note else ""}</text>')
    return svg_wrap(w, h, title, "\n".join(out))


def line_chart(title, xs_label, series, w=720, h=340, unit=""):
    """series: (name, colour, [(x, y)])."""
    left, right, top, bot = 64, 20, 56, 60
    pw, ph = w - left - right, h - top - bot
    pts = [p for _, _, ps_ in series for p in ps_]
    if not pts:
        return svg_wrap(w, h, title, f'<text x="{w / 2}" y="{h / 2}" text-anchor="middle" fill="#57606a">NOT RUN</text>')
    xmax = max(p[0] for p in pts)
    ymax = max(p[1] for p in pts) * 1.1
    out = []
    for i in range(5):
        y = top + ph - ph * i / 4
        out.append(f'<line x1="{left}" y1="{y:.1f}" x2="{w - right}" y2="{y:.1f}" stroke="#eaeef2"/>'
                   f'<text x="{left - 6}" y="{y + 4:.1f}" text-anchor="end" fill="#57606a">{ymax * i / 4:,.0f}</text>')
    for xv in sorted({p[0] for p in pts}):
        x = left + pw * xv / xmax
        out.append(f'<text x="{x:.1f}" y="{h - bot + 16}" text-anchor="middle" fill="#57606a">{xv:,.0f}</text>')
    lx = left
    for name, colr, ps_ in series:
        pp = " ".join(f"{left + pw * x / xmax:.1f},{top + ph - ph * y / ymax:.1f}" for x, y in ps_)
        out.append(f'<polyline points="{pp}" fill="none" stroke="{colr}" stroke-width="2.2"/>')
        for x, y in ps_:
            out.append(f'<circle cx="{left + pw * x / xmax:.1f}" cy="{top + ph - ph * y / ymax:.1f}" r="3.2" fill="{colr}"/>')
        out.append(f'<rect x="{lx}" y="{h - 26}" width="12" height="12" fill="{colr}" rx="2"/><text x="{lx + 18}" y="{h - 16}" fill="#1f2328">{esc(name)}</text>')
        lx += 28 + 6.3 * len(name)
    out.append(f'<text x="{w - right}" y="{h - 36}" text-anchor="end" fill="#57606a" font-size="11">x: {esc(xs_label)}   y: {esc(unit)}</text>')
    return svg_wrap(w, h, title, "\n".join(out))


def charts(S, outdir):
    os.makedirs(outdir, exist_ok=True)
    written = []

    def put(name, svg):
        with open(os.path.join(outdir, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(svg)
        written.append(name)
    groups = [ml for _, ml in MODELS]
    ser = [(bl, COL[b], [(S["text"].get((m, b)) or (None,))[0] and S["text"][(m, b)][0].get("decode_tokens_per_second_median") for m, _ in MODELS]) for b, bl in BACKENDS]
    put("chart-decode-tps.svg", grouped_bars("Decode throughput (higher is better)", groups, ser, "tokens/s, median of 5", nd=1))
    ser = [(bl, COL[b], [(S["text"].get((m, b)) or (None,))[0] and S["text"][(m, b)][0].get("ttft_ms_median") for m, _ in MODELS]) for b, bl in BACKENDS]
    put("chart-ttft.svg", grouped_bars("Warm time to first token, ~200-225-token prompt (lower is better)", groups, ser, "ms, median of 5", nd=0))
    lines = []
    for m, ml in MODELS:
        for b, bl in BACKENDS:
            t = S["text"].get((m, b))
            if t:
                pts = [(p["target_prompt_tokens"], p["ttft_ms_median"] / 1000) for p in t[0].get("prefill_sweep", []) if p.get("result_status") == "PASS"]
                if pts:
                    lines.append((f"{ml.split()[0]} {b}", COL[b] if m == MODELS[1][0] else ("#3f9d73" if b == "vulkan" else "#8a8f98"), pts))
    put("chart-prefill-ttft.svg", line_chart("Prefill: time to first token vs prompt length (lower is better)", "target prompt tokens (real counts in the table)", lines, unit="seconds"))
    img_groups, img_ser = [], {b: [] for b, _ in BACKENDS}
    for tag, lab in (("sd15", "SD1.5 20 steps"), ("flux3-fast", "FLUX.3 fast (6)"), ("flux3-quality", "FLUX.3 quality (16)")):
        img_groups.append(lab)
        for b, _ in BACKENDS:
            j = S["image"].get((tag, b))
            st = image_stats(j) if j else None
            img_ser[b].append(st["median_s"] if st and st["median_s"] is not None else None)
    put("chart-image-seconds.svg", grouped_bars("512x512 image generation, wall time per process (lower is better)", img_groups,
                                               [(bl, COL[b], img_ser[b]) for b, bl in BACKENDS], "seconds, median", nd=0, note="n per bar in table"))
    kvg, kvs = [], {ty: [] for ty in ("f16", "q8_0", "q4_0")}
    for b, bl in BACKENDS:
        for ctx in (4096, 8192):
            kvg.append(f"{b} {ctx // 1024}K")
            for ty in kvs:
                k = S["kv"].get((b, ty, ctx))
                kvs[ty].append(mib(k["peak_commit_bytes"]) if k and k["rc"] == 0 and k.get("peak_commit_bytes") else None)
    put("chart-kv-commit.svg", grouped_bars("Peak process commit by KV-cache type (lower is better)", kvg, [(ty, COL[ty], v) for ty, v in kvs.items()],
                                           "MiB, qwen3-0.6b Q4_K_M", nd=0, note="includes weights+compute buffers"))
    kvg2, kvs2 = [], {ty: [] for ty in ("f16", "q8_0", "q4_0")}
    for b, bl in BACKENDS:
        for ctx in (4096, 8192):
            for ty in kvs2:
                k = S["kv"].get((b, ty, ctx))
                r = lb_value(k, "pp") if k and k["rc"] == 0 else None
                kvs2[ty].append(ctx / r["avg_ts"] if r else None)
    put("chart-kv-ttft.svg", grouped_bars("Prefill time of a full context by KV-cache type (lower is better)", kvg, [(ty, COL[ty], v) for ty, v in kvs2.items()],
                                         "seconds = context / pp t/s", nd=2))
    return written


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--charts", required=True)
    ap.add_argument("--tables", required=True)
    ap.add_argument("--readme-table", required=True)
    a = ap.parse_args()
    S = collect(a.data)
    sm = summary(S)
    with open(os.path.join(a.data, "summary.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump({"schema_version": 1, "metrics": sm}, f, indent=2, sort_keys=True)
        f.write("\n")
    with open(a.tables, "w", encoding="utf-8", newline="\n") as f:
        f.write(tables(S))
    with open(a.readme_table, "w", encoding="utf-8", newline="\n") as f:
        f.write(readme_table(S))
    print("metrics:", len(sm), "charts:", charts(S, a.charts))


if __name__ == "__main__":
    main()
