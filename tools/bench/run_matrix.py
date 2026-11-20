#!/usr/bin/env python3
"""Wave-D benchmark driver. Runs ONE section at a time (never concurrent heavy jobs) and writes raw JSON.

Paths come from environment variables (no personal paths are stored in the repository):
  HCS_EXE_CPU     static CPU HCSInfer.exe            HCS_EXE_VK   static Vulkan HCSInfer.exe
  HCS_LLAMA_BENCH_CPU / HCS_LLAMA_BENCH_VK   llama-bench.exe of the pinned llama.cpp (CPU / Vulkan builds)
  HCS_MODELS      model root (contains llm/LFM3-Omni-2B, diffusion/..)   HCS_QWEN3   qwen3-0.6b-q4_k_m.gguf
Usage: run_matrix.py <section> --out <dir>   sections: env hdd text llamabench kv vision sd flux api
"""
from __future__ import annotations
import argparse, datetime, json, os, platform, re, shutil, statistics, subprocess, sys, threading, time, urllib.request
sys.path.insert(0, os.path.dirname(__file__))
import bench_common as bc


def env(name):
    v = os.environ.get(name)
    if not v:
        sys.exit(f"missing environment variable {name}")
    return v


def now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def med_p90(xs):
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None, None
    if len(xs) == 1:
        return xs[0], xs[0]
    return statistics.median(xs), xs[int(round(0.9 * (len(xs) - 1)))]


def lfm3():
    return os.path.join(env("HCS_MODELS"), "llm", "LFM3-Omni-2B", "LFM3-Omni-2B-Q8_0.gguf")


def exe(b):
    return env("HCS_EXE_CPU") if b == "cpu" else env("HCS_EXE_VK")


def ps(c):
    return subprocess.run(["powershell", "-NoProfile", "-Command", c], capture_output=True, text=True).stdout.strip()


def sec_env(a):
    d = {"timestamp_utc": now(), "os": platform.platform(), "python": sys.version.split()[0]}
    for k, p in (("exe_cpu", "HCS_EXE_CPU"), ("exe_vulkan", "HCS_EXE_VK")):
        d[k] = {"file": os.path.basename(env(p)), "sha256": bc.sha256(env(p)), "bytes": os.path.getsize(env(p))}
    r = bc.run([env("HCS_EXE_VK"), "doctor", "--json", "--storage-root", env("HCS_MODELS")])
    try:
        d["doctor"] = json.loads(r["stdout"])
    except Exception:
        d["doctor_error"] = r["stderr"][-500:]
    d["power_scheme"] = ps("powercfg /getactivescheme")
    d["battery_or_ac"] = ps("$b=Get-CimInstance Win32_Battery; if($b){'battery present, status '+$b.BatteryStatus}else{'no battery (AC powered)'}")
    d["disk_of_models"] = ps("$p=Get-Partition -DriveLetter ('%s'[0]); (Get-Disk -Number $p.DiskNumber | Select FriendlyName,BusType,Size | ConvertTo-Json -Compress)" % env("HCS_MODELS"))
    d["free_ram_gib_at_start"] = ps("[math]::Round((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory/1MB,2)")
    d["cpu_temperature"] = "not measured (no sensor interface without extra drivers)"
    d["git_commit"] = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    bc.write_json(os.path.join(a.out, "environment.json"), d)


def sec_hdd(a):
    """Read-only sequential reads (doctor --read-file). Each file is read once first (likely cold: files not used in this session),
    then the first file is read 3 more times (page-cache warm). The OS cache is never flushed, so 'cold' is best effort."""
    root = env("HCS_MODELS")
    files = [os.path.join(root, "text", "LFM3-Omni-2B", "LFM3-Omni-2B-Q8_0.gguf"),
             os.path.join(root, "diffusion", "flux3", "qwen3vl", "Qwen3VL-4B-Instruct-Q4_K_M.gguf"),
             os.path.join(root, "vision", "SmolVLM2-500M-Video-Instruct-Q8_0.gguf")]
    res = []
    for i, (f, reps) in enumerate([(x, 1) for x in files] + [(files[0], 3)]):
        r = bc.run([env("HCS_EXE_VK"), "doctor", "--json", "--no-vulkan", "--storage-root", root, "--read-file", f,
                    "--read-max-mib", "1024", "--read-repeats", str(reps)], timeout=900)
        d = json.loads(r["stdout"]) if r["rc"] == 0 else None
        res.append({"file": os.path.basename(f), "phase": "first-read (likely cold)" if i < len(files) else "repeat (page-cache warm)",
                    "rc": r["rc"], "read_benchmark": d and d["read_benchmark"], "storage": d and d["storage"]["requested_volume"]})
        print("hdd", os.path.basename(f), r["rc"], d and d["read_benchmark"].get("mib_s_median"))
    bc.write_json(os.path.join(a.out, "hdd-read.json"), {
        "timestamp_utc": now(), "note": "read-only sequential, up to 1 GiB per sample, 4 MiB buffer; OS file cache NOT flushed (needs administrator rights)",
        "results": res})


def sec_text(a):
    """HCSInfer benchmark --suite full, N iterations; CPU and Vulkan, qwen3-0.6b Q4_K_M and LFM3-Omni-2B Q8_0."""
    models = {"qwen3-0.6b-q4_k_m": env("HCS_QWEN3"), "lfm3-omni-2b-q8_0": lfm3()}
    for tag, path in models.items():
        for be in a.backends:
            out = os.path.join(a.out, f"text-full-{be}-{tag}.json")
            if os.path.exists(out) and not a.force:
                print("skip", out)
                continue
            cmd = [exe(be), "benchmark", "--suite", "full", "--model", path, "--backend", be, "--iterations", str(a.iterations),
                   "--warmup", "1", "--max-tokens", "64", "--sweep-sizes", a.sweep, "--ctx", "4096", "--export", out, "--force"]
            r = bc.run(cmd, timeout=3600)
            print(tag, be, "rc", r["rc"], f'{r["wall_s"]:.0f}s')
            bc.write_json(out[:-5] + ".run.json", {k: r[k] for k in ("rc", "wall_s", "timed_out", "peak_ws_bytes", "peak_commit_bytes")}
                          | {"stderr_tail": r["stderr"][-600:]})


def llama_bench(be, model, extra, reps, timeout=3600):
    b = env("HCS_LLAMA_BENCH_CPU" if be == "cpu" else "HCS_LLAMA_BENCH_VK")
    cmd = [b, "-m", model, "-o", "json", "-r", str(reps)] + extra + (["-t", "8"] if be == "cpu" else ["-ngl", "99"])
    r = bc.run(cmd, timeout=timeout, cwd=os.path.dirname(b))
    try:
        js = json.loads(r["stdout"])
    except Exception:
        js = None
    return r, js


def sec_llamabench(a):
    models = {"qwen3-0.6b-q4_k_m": env("HCS_QWEN3"), "lfm3-omni-2b-q8_0": lfm3()}
    for tag, path in models.items():
        for be in a.backends:
            out = os.path.join(a.out, f"llamabench-{be}-{tag}.json")
            r, js = llama_bench(be, path, ["-p", "512", "-n", "128"], a.iterations)
            bc.write_json(out, {"timestamp_utc": now(), "model_file": os.path.basename(path), "model_sha256": bc.sha256(path), "backend": be,
                                "reps": a.iterations, "rc": r["rc"], "wall_s": r["wall_s"], "peak_ws_bytes": r["peak_ws_bytes"],
                                "peak_commit_bytes": r["peak_commit_bytes"], "results": js, "stderr_tail": r["stderr"][-400:]})
            print("llamabench", tag, be, r["rc"])


def sec_kv(a):
    """KV cache types x context size with llama-bench (the HCS engine does not expose cache types)."""
    path = env("HCS_QWEN3")
    for be in a.backends:
        for ctx in (4096, 8192):
            for ty in ("f16", "q8_0", "q4_0"):
                out = os.path.join(a.out, f"kv-{be}-{ty}-{ctx}.json")
                if os.path.exists(out) and not a.force:
                    print("skip", out)
                    continue
                r, js = llama_bench(be, path, ["-p", str(ctx), "-n", "0", "-fa", "1", "-ctk", ty, "-ctv", ty], a.iterations)
                bc.write_json(out, {"timestamp_utc": now(), "model_file": os.path.basename(path), "backend": be, "cache_type": ty,
                                    "context": ctx, "reps": a.iterations, "rc": r["rc"], "wall_s": r["wall_s"],
                                    "peak_ws_bytes": r["peak_ws_bytes"], "peak_commit_bytes": r["peak_commit_bytes"],
                                    "results": js, "stderr_tail": r["stderr"][-500:]})
                print("kv", be, ty, ctx, r["rc"], f'{r["wall_s"]:.0f}s')


def sec_vision(a):
    img = os.path.join(a.out, "vision-test-image.png")
    bc.make_test_png(img)
    model = lfm3()
    for be in a.backends:
        runs = []
        for i in range(a.iterations):
            r = bc.run([exe(be), "vision", "--model", model, "--image", img, "--prompt", "Describe this image in one sentence.",
                        "--backend", be, "--max-tokens", "48", "--temp", "0", "--seed", "42", "--json"], timeout=900)
            try:
                j = json.loads(r["stdout"])
            except Exception:
                j = None
            runs.append({"rc": r["rc"], "wall_s": r["wall_s"], "peak_ws_bytes": r["peak_ws_bytes"], "peak_commit_bytes": r["peak_commit_bytes"],
                         "metrics": j and j.get("metrics"), "state": j and j.get("state"), "device": j and j.get("device"),
                         "text_chars": len(j["text"]) if j and j.get("text") else None})
            print("vision", be, i, r["rc"], runs[-1]["metrics"] and runs[-1]["metrics"].get("ttft_ms"))
        bc.write_json(os.path.join(a.out, f"vision-{be}-lfm3-omni-2b-q8_0.json"), {
            "timestamp_utc": now(), "backend": be, "model_file": os.path.basename(model), "model_sha256": bc.sha256(model),
            "image_sha256": bc.sha256(img), "image": "512x512 synthetic PNG generated by tools/bench/bench_common.py",
            "note": "each run is a separate process: load_ms is a process-cold-ish load (file cache not flushed)", "n": a.iterations, "runs": runs})


def _image_runs(a, model, be, extra, tag, meta):
    runs = []
    for i in range(a.iterations):
        png = os.path.join(a.out, "images", f"{tag}-{be}-{i}.png")
        os.makedirs(os.path.dirname(png), exist_ok=True)
        r = bc.run([exe(be), "image", "generate", "--model", model, "--prompt", "a red apple on a wooden table, studio photo",
                    "--width", "512", "--height", "512", "--seed", "42", "--backend", be, "--output", png, "--force", "--json"] + extra,
                   timeout=a.timeout)
        try:
            j = json.loads(r["stdout"])
        except Exception:
            j = None
        runs.append({"rc": r["rc"], "wall_s": r["wall_s"], "timed_out": r["timed_out"], "peak_ws_bytes": r["peak_ws_bytes"],
                     "peak_commit_bytes": r["peak_commit_bytes"], "result": j, "png_sha256": bc.sha256(png) if os.path.exists(png) else None,
                     "stderr_tail": r["stderr"][-300:] if r["rc"] else ""})
        print(tag, be, i, r["rc"], f'{r["wall_s"]:.0f}s')
        bc.write_json(os.path.join(a.out, f"image-{tag}-{be}.json"), {"timestamp_utc": now(), "backend": be, "n": len(runs), "runs": runs} | meta)


def sec_sd(a):
    m = os.path.join(env("HCS_MODELS"), "diffusion", "sd15-q8_0.gguf")
    for be in a.backends:
        _image_runs(a, m, be, ["--steps", "20"], "sd15", {"model_file": os.path.basename(m), "model_sha256": bc.sha256(m),
                                                         "size": "512x512", "steps": 20, "seed": 42})


def sec_flux(a):
    m = os.path.join(env("HCS_MODELS"), "diffusion", "flux3", "flux-3-image-klein-UD-Q4_K_XL.gguf")
    sha = bc.sha256(m)
    for be in a.backends:
        for preset in a.presets:
            _image_runs(a, m, be, ["--preset", preset], f"flux3-{preset}", {"model_file": os.path.basename(m), "model_sha256": sha,
                                                                           "size": "512x512", "preset": preset, "seed": 42})


def sec_api(a):
    """HCSInfer serve: HTTP TTFT of streaming chat, non-stream latency, first-request model load."""
    be = a.backends[0]
    root = os.path.join(env("HCS_MODELS"), "llm", "LFM3-Omni-2B")
    tok = "benchmark-token-" + "x" * 16
    envv = dict(os.environ, HCS_API_TOKEN=tok)
    subprocess.run([exe(be), "models", "scan", root], capture_output=True, env=envv)
    p = subprocess.Popen([exe(be), "serve", "--port", "0", "--model-root", root], stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=envv)
    lines, port = [], None

    def rd():
        for ln in iter(p.stderr.readline, b""):
            lines.append(ln.decode("utf-8", "replace"))
    threading.Thread(target=rd, daemon=True).start()
    for _ in range(100):
        time.sleep(0.2)
        for ln in lines:
            mm = re.search(r"127\.0\.0\.1:(\d+)", ln)
            if mm:
                port = int(mm.group(1))
        if port:
            break
    if not port:
        p.kill()
        sys.exit("serve did not start: " + "".join(lines)[-400:])
    base = f"http://127.0.0.1:{port}"

    def req(path, body=None):
        rq = urllib.request.Request(base + path, data=json.dumps(body).encode() if body is not None else None,
                                    headers={"Authorization": "Bearer " + tok, "Content-Type": "application/json"})
        return urllib.request.urlopen(rq, timeout=600)
    data = json.loads(req("/v1/models").read())["data"]
    mid = [m["id"] for m in data if "lfm3" in m["id"].lower()][0]
    msgs = [{"role": "user", "content": "Reply with one short sentence about the sea."}]
    t = time.perf_counter()
    req("/v1/chat/completions", {"model": mid, "messages": msgs, "max_tokens": 1, "temperature": 0, "seed": 42}).read()
    load_first_ms = (time.perf_counter() - t) * 1000
    ttfts, totals, toks, nonstream = [], [], [], []
    for i in range(a.iterations + 1):  # first = warm-up, discarded
        t = time.perf_counter()
        resp = req("/v1/chat/completions", {"model": mid, "messages": msgs, "max_tokens": 48, "temperature": 0, "seed": 42, "stream": True})
        first, n = None, 0
        for raw in resp:
            ln = raw.decode("utf-8", "replace").strip()
            if ln.startswith("data:") and ln != "data: [DONE]":
                try:
                    ch = json.loads(ln[5:])
                except Exception:
                    continue
                if ch.get("choices") and ch["choices"][0].get("delta", {}).get("content"):
                    n += 1
                    if first is None:
                        first = (time.perf_counter() - t) * 1000
        tot = (time.perf_counter() - t) * 1000
        if i:
            ttfts.append(first)
            totals.append(tot)
            toks.append(n)
    for _ in range(a.iterations):
        t = time.perf_counter()
        req("/v1/chat/completions", {"model": mid, "messages": msgs, "max_tokens": 48, "temperature": 0, "seed": 42}).read()
        nonstream.append((time.perf_counter() - t) * 1000)
    m = bc._mem(p._handle)
    p.terminate()
    try:
        p.wait(10)
    except Exception:
        p.kill()
    f = lambda xs: dict(zip(("median", "p90"), med_p90(xs))) | {"samples": xs}
    bc.write_json(os.path.join(a.out, f"api-latency-{be}-lfm3-omni-2b-q8_0.json"), {
        "timestamp_utc": now(), "backend": be, "model_id": mid, "n": a.iterations, "first_request_incl_model_load_ms": load_first_ms,
        "stream_ttft_ms": f(ttfts), "stream_total_ms": f(totals), "stream_content_chunks": toks, "nonstream_total_ms": f(nonstream),
        "server_peak_ws_bytes": m and m[0], "server_peak_commit_bytes": m and m[1],
        "note": "loopback HTTP, bearer token, fixed prompt, temperature 0, seed 42, 48 max new tokens; TTFT = request sent -> first non-empty content delta"})
    print("api ttft median", med_p90(ttfts)[0])


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("section", choices="env hdd text llamabench kv vision sd flux api".split())
    ap.add_argument("--out", required=True)
    ap.add_argument("--backends", nargs="+", default=["cpu", "vulkan"])
    ap.add_argument("--iterations", type=int, default=5)
    ap.add_argument("--sweep", default="128,512,2048")
    ap.add_argument("--presets", nargs="+", default=["fast", "quality"])
    ap.add_argument("--timeout", type=int, default=3600)
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    globals()["sec_" + a.section](a)
