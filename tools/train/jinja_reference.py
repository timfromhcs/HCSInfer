#!/usr/bin/env python3
"""Reference renderer: the LFM3 chat template embedded in the GGUF, rendered with Jinja2 (HF-style environment).

Used to pin the golden strings of tests/unit/train/test_chat_template.cpp and, with --check, to compare the C++
renderer (via `HCSTrain render` output files) against Jinja on a JSONL of conversations.
  python -I jinja_reference.py --gguf model.gguf --records records.jsonl --out golden.jsonl [--generation-prompt]
"""
import argparse
import json
import os
import sys

import jinja2
from jinja2.ext import Extension
from jinja2.sandbox import ImmutableSandboxedEnvironment

HERE = os.path.dirname(os.path.abspath(__file__))


def load_template(gguf_path):
    from gguf import GGUFReader
    r = GGUFReader(gguf_path)
    return r.fields["tokenizer.chat_template"].contents()


class _Gen(Extension):
    tags = {"generation"}

    def parse(self, parser):
        lineno = next(parser.stream).lineno
        body = parser.parse_statements(["name:endgeneration"], drop_needle=True)
        return jinja2.nodes.CallBlock(self.call_method("_pass", []), [], [], body).set_lineno(lineno)

    def _pass(self, caller):
        return caller()


def render(template_text, messages, tools, add_generation_prompt, bos="<|startoftext|>"):
    def raise_exception(m):
        raise jinja2.exceptions.TemplateError(m)

    def tojson(x, ensure_ascii=False, indent=None, separators=None, sort_keys=False):
        return json.dumps(x, ensure_ascii=ensure_ascii, indent=indent, separators=separators, sort_keys=sort_keys)

    env = ImmutableSandboxedEnvironment(trim_blocks=True, lstrip_blocks=True, extensions=[_Gen])
    env.filters["tojson"] = tojson
    env.globals["raise_exception"] = raise_exception
    return env.from_string(template_text).render(messages=messages, tools=tools or None, bos_token=bos,
                                                 add_generation_prompt=add_generation_prompt)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gguf", required=True)
    ap.add_argument("--records", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--generation-prompt", action="store_true")
    a = ap.parse_args()
    t = load_template(a.gguf)
    with open(a.records, encoding="utf-8") as f, open(a.out, "w", encoding="utf-8") as o:
        for line in f:
            if not line.strip():
                continue
            r = json.loads(line)
            msgs = r["messages"]
            for m in msgs:  # the template wants arguments as mappings
                for tc in m.get("tool_calls") or []:
                    fn = tc.get("function", tc)
                    if isinstance(fn.get("arguments"), str):
                        fn["arguments"] = json.loads(fn["arguments"])
            o.write(json.dumps({"id": r.get("id"), "text": render(t, msgs, r.get("tools"), a.generation_prompt)},
                               ensure_ascii=False) + "\n")


if __name__ == "__main__":
    sys.exit(main())
