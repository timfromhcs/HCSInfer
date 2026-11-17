#!/usr/bin/env python3
"""Held-out agentic eval for HCSInfer: tool calling, tool-result follow-up and small coding tasks.

Runs tasks.jsonl against an OpenAI-compatible endpoint (HCSInfer `serve`) and scores every task automatically:

  single / select / typed / parallel : the response must contain the expected tool call(s) (name + arguments).
  nocall                             : the response must contain NO tool call (and optional text matches).
  followup                           : after a tool result the model must answer in text containing the facts.
  coding                             : the extracted Python code is run against assert tests in a restricted
                                       subprocess (AST allow-list for imports, 5 s timeout, `python -I`).

Usage:
  python score.py --base-url http://127.0.0.1:8765 --token <t> --out results.json   # live run
  python score.py --replay responses.jsonl --out results.json                       # re-score saved answers
  python score.py --selftest                                                        # scorer logic, no model

This file and tasks.jsonl are a HELD-OUT evaluation set: never train or tune a model on them.
Standard library only.
"""
import argparse
import ast
import hashlib
import json
import operator
import os
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ALLOWED_IMPORTS = {"math", "re", "itertools", "collections", "functools", "string", "json", "heapq", "bisect",
                   "typing", "operator"}


# ---- argument matching ---------------------------------------------------------------------------
_OPS = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul, ast.Div: operator.truediv,
        ast.Pow: operator.pow, ast.USub: operator.neg, ast.Mod: operator.mod}


def _safe_eval(node):
    if isinstance(node, ast.Expression):
        return _safe_eval(node.body)
    if isinstance(node, ast.Constant) and isinstance(node.value, (int, float)):
        return node.value
    if isinstance(node, ast.BinOp) and type(node.op) in _OPS:
        return _OPS[type(node.op)](_safe_eval(node.left), _safe_eval(node.right))
    if isinstance(node, ast.UnaryOp) and type(node.op) in _OPS:
        return _OPS[type(node.op)](_safe_eval(node.operand))
    raise ValueError("not arithmetic")


def arith_value(expr):
    try:
        return _safe_eval(ast.parse(str(expr).replace("^", "**").replace("x", "*").strip(), mode="eval"))
    except Exception:
        return None


def _norm(v):
    return v.strip().lower() if isinstance(v, str) else v


def match_value(expected, actual):
    if isinstance(expected, dict):
        if "contains" in expected:
            return isinstance(actual, str) and expected["contains"].lower() in actual.lower()
        if "ci" in expected:
            return isinstance(actual, str) and _norm(actual) == _norm(expected["ci"])
        if "ci_any" in expected:
            return isinstance(actual, str) and _norm(actual) in [_norm(x) for x in expected["ci_any"]]
        if "calc" in expected:
            want = arith_value(expected["calc"])
            got = arith_value(actual) if isinstance(actual, str) else actual
            return want is not None and got is not None and abs(float(got) - float(want)) < 1e-9
        return False
    if isinstance(expected, bool) or isinstance(actual, bool):
        return expected is actual
    if isinstance(expected, (int, float)) and isinstance(actual, (int, float)):
        return abs(float(expected) - float(actual)) < 1e-9
    if isinstance(expected, str) and isinstance(actual, str):
        return _norm(expected) == _norm(actual)
    if isinstance(expected, list) and isinstance(actual, list):
        return len(expected) == len(actual) and all(match_value(e, a) for e, a in zip(expected, actual))
    return expected == actual


def match_call(exp, got):
    if got["name"] != exp["name"]:
        return False
    args = got.get("arguments") or {}
    return all(k in args and match_value(v, args[k]) for k, v in exp["arguments"].items())


# ---- coding ---------------------------------------------------------------------------------------
def extract_code(text):
    m = re.findall(r"```(?:python|py)?\s*\n(.*?)```", text or "", re.S)
    return (m[0] if m else (text or "")).strip()


def code_is_allowed(src):
    try:
        tree = ast.parse(src)
    except SyntaxError as e:
        return False, "syntax error: %s" % e
    for n in ast.walk(tree):
        if isinstance(n, ast.Import):
            for a in n.names:
                if a.name.split(".")[0] not in ALLOWED_IMPORTS:
                    return False, "import not allowed: " + a.name
        elif isinstance(n, ast.ImportFrom):
            if (n.module or "").split(".")[0] not in ALLOWED_IMPORTS:
                return False, "import not allowed: " + str(n.module)
        elif isinstance(n, ast.Name) and n.id in {"open", "exec", "eval", "compile", "__import__", "input"}:
            return False, "builtin not allowed: " + n.id
        elif isinstance(n, ast.Attribute) and n.attr.startswith("__"):
            return False, "dunder attribute not allowed"
    return True, ""


def run_python_tests(src, fname, tests, timeout=5):
    ok, why = code_is_allowed(src)
    if not ok:
        return False, why
    prog = src + "\n\n" + "\n".join(tests) + "\nprint('PASS')\n"
    if ("def " + fname) not in src:
        return False, "function %s not defined" % fname
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "t.py")
        with open(p, "w", encoding="utf-8") as f:
            f.write(prog)
        try:
            r = subprocess.run([sys.executable, "-I", p], capture_output=True, text=True, timeout=timeout, cwd=d)
        except subprocess.TimeoutExpired:
            return False, "timeout"
    if r.returncode == 0 and "PASS" in r.stdout:
        return True, ""
    return False, (r.stderr.strip().splitlines() or ["failed"])[-1][:200]


# ---- scoring --------------------------------------------------------------------------------------
def score_task(task, resp):
    """resp: {"content": str|None, "tool_calls": [{"name":..,"arguments":{..}}], "error": str|None}"""
    exp = task["expect"]
    if resp.get("error"):
        return False, "request error: " + resp["error"]
    calls = resp.get("tool_calls") or []
    text = resp.get("content") or ""
    if "python_function" in exp:
        return run_python_tests(extract_code(text), exp["python_function"], exp["tests"])
    if exp.get("no_call"):
        if calls:
            return False, "unexpected tool call: " + calls[0]["name"]
        low = text.lower()
        if "contains_any" in exp and not any(s.lower() in low for s in exp["contains_any"]):
            return False, "answer misses all of %s" % exp["contains_any"]
        if "contains_all" in exp and not all(s.lower() in low for s in exp["contains_all"]):
            return False, "answer misses one of %s" % exp["contains_all"]
        if not text.strip():
            return False, "empty answer"
        return True, ""
    want = exp["calls"]
    if len(calls) != len(want):
        return False, "expected %d call(s), got %d" % (len(want), len(calls))
    if exp.get("unordered"):
        pool = list(calls)
        for w in want:
            hit = next((c for c in pool if match_call(w, c)), None)
            if hit is None:
                return False, "no call matches " + w["name"] + json.dumps(w["arguments"])
            pool.remove(hit)
        return True, ""
    for w, c in zip(want, calls):
        if not match_call(w, c):
            return False, "call mismatch: got %s%s" % (c["name"], json.dumps(c.get("arguments")))
    return True, ""


def gold_response(task):
    exp = task["expect"]
    if "python_function" in exp:
        gold = {"is_palindrome": "import re\ndef is_palindrome(s):\n    t = re.sub('[^a-z0-9]', '', s.lower())\n    return t == t[::-1]",
                "fizzbuzz": "def fizzbuzz(n):\n    return ['FizzBuzz' if i % 15 == 0 else 'Fizz' if i % 3 == 0 else 'Buzz' if i % 5 == 0 else str(i) for i in range(1, n + 1)]",
                "flatten": "def flatten(l):\n    o = []\n    for x in l:\n        o += flatten(x) if isinstance(x, list) else [x]\n    return o",
                "word_count": "def word_count(t):\n    d = {}\n    for w in t.lower().split():\n        d[w] = d.get(w, 0) + 1\n    return d",
                "two_sum": "def two_sum(n, t):\n    for i in range(len(n)):\n        for j in range(i + 1, len(n)):\n            if n[i] + n[j] == t:\n                return (i, j)\n    return None",
                "gcd": "def gcd(a, b):\n    while b:\n        a, b = b, a % b\n    return a",
                "roman_to_int": "def roman_to_int(s):\n    v = dict(I=1, V=5, X=10, L=50, C=100, D=500, M=1000)\n    t = 0\n    for i, c in enumerate(s):\n        t += -v[c] if i + 1 < len(s) and v[c] < v[s[i + 1]] else v[c]\n    return t",
                "chunk": "def chunk(l, n):\n    return [l[i:i + n] for i in range(0, len(l), n)]"}
        return {"content": "```python\n" + gold[exp["python_function"]] + "\n```", "tool_calls": []}
    if exp.get("no_call"):
        return {"content": " ".join(exp.get("contains_all", []) + exp.get("contains_any", [])[:1] or ["ok"]), "tool_calls": []}

    def conc(v):
        if isinstance(v, dict):
            if "contains" in v:
                return v["contains"]
            if "ci" in v:
                return v["ci"]
            if "ci_any" in v:
                return v["ci_any"][0]
            if "calc" in v:
                return v["calc"]
        return v
    return {"content": None, "tool_calls": [{"name": c["name"], "arguments": {k: conc(v) for k, v in c["arguments"].items()}}
                                            for c in exp["calls"]]}


def load_tasks(path=None):
    with open(path or os.path.join(HERE, "tasks.jsonl"), encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def selftest():
    tasks = load_tasks()
    bad = 0
    for t in tasks:
        ok, why = score_task(t, gold_response(t))
        if not ok:
            bad += 1
            print("selftest: gold answer rejected for", t["id"], why)
    # negative controls: an empty answer must fail every task
    for t in tasks:
        ok, _ = score_task(t, {"content": "", "tool_calls": []})
        if ok:
            bad += 1
            print("selftest: empty answer accepted for", t["id"])
    print("selftest: %d tasks, %d problems" % (len(tasks), bad))
    return 1 if bad else 0


# ---- live endpoint ---------------------------------------------------------------------------------
def call_endpoint(base, token, task, model, max_tokens, timeout, stream=False):
    body = {"messages": task["messages"], "temperature": 0, "max_tokens": max_tokens, "stream": stream}
    if model:
        body["model"] = model
    if task["tools"]:
        body["tools"] = task["tools"]
        body["tool_choice"] = "auto"
    req = urllib.request.Request(base.rstrip("/") + "/v1/chat/completions", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json",
                                          **({"Authorization": "Bearer " + token} if token else {})})
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return {"error": "HTTP %d %s" % (e.code, e.read().decode("utf-8", "replace")[:200]), "seconds": time.time() - t0}
    except Exception as e:  # noqa: BLE001
        return {"error": repr(e), "seconds": time.time() - t0}
    msg = json.loads(raw)["choices"][0]["message"]
    calls = []
    for tc in msg.get("tool_calls") or []:
        try:
            args = json.loads(tc["function"]["arguments"])
        except Exception:  # noqa: BLE001
            args = {}
        calls.append({"name": tc["function"]["name"], "arguments": args})
    return {"content": msg.get("content"), "tool_calls": calls, "seconds": time.time() - t0}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-url")
    ap.add_argument("--token", default=os.environ.get("HCS_API_TOKEN", ""))
    ap.add_argument("--model", default="")
    ap.add_argument("--tasks", default=None)
    ap.add_argument("--replay")
    ap.add_argument("--save-responses")
    ap.add_argument("--out")
    ap.add_argument("--max-tokens", type=int, default=400)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    tasks = load_tasks(a.tasks)
    saved = {}
    if a.replay:
        with open(a.replay, encoding="utf-8") as f:
            for line in f:
                if line.strip():
                    o = json.loads(line)
                    saved[o["id"]] = o["response"]
    elif not a.base_url:
        ap.error("--base-url or --replay required")
    results, by_cat = [], {}
    out_resp = []
    for t in tasks:
        resp = saved.get(t["id"]) if a.replay else call_endpoint(a.base_url, a.token, t, a.model, a.max_tokens, a.timeout)
        if resp is None:
            resp = {"error": "no saved response", "tool_calls": []}
        ok, why = score_task(t, resp)
        results.append({"id": t["id"], "category": t["category"], "pass": ok, "reason": why,
                        "seconds": resp.get("seconds")})
        out_resp.append({"id": t["id"], "response": resp})
        c = by_cat.setdefault(t["category"], [0, 0])
        c[0] += int(ok)
        c[1] += 1
        print("%-12s %s %s" % (t["id"], "PASS" if ok else "FAIL", why))
    total = sum(r["pass"] for r in results)
    tool_cats = [c for c in by_cat if c != "coding"]
    tool_pass = sum(by_cat[c][0] for c in tool_cats)
    tool_n = sum(by_cat[c][1] for c in tool_cats)
    summary = {"tasks": len(results), "passed": total, "score": round(total / len(results), 4),
               "tool_use_score": round(tool_pass / tool_n, 4) if tool_n else None,
               "coding_score": round(by_cat["coding"][0] / by_cat["coding"][1], 4) if "coding" in by_cat else None,
               "by_category": {k: {"passed": v[0], "total": v[1]} for k, v in by_cat.items()},
               "tasks_sha256": hashlib.sha256(open(a.tasks or os.path.join(HERE, "tasks.jsonl"), "rb").read()).hexdigest()}
    print(json.dumps(summary, indent=2))
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            json.dump({"summary": summary, "results": results}, f, indent=2)
    if a.save_responses:
        with open(a.save_responses, "w", encoding="utf-8") as f:
            for o in out_resp:
                f.write(json.dumps(o, ensure_ascii=False) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
