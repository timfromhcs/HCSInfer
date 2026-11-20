#!/usr/bin/env python3
"""Markdown link checker for the repository's own documentation.

Checks every relative link / image in tracked-style Markdown files: the target file or directory must exist and, for
`file.md#anchor` / `#anchor`, the anchor must match a heading in the target (GitHub slug rules). http(s) links are
not fetched (offline check); they are only counted. Exit code 1 when anything is broken.

    python tools/check_links.py [--include-evidence]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SKIP_DIRS = {"external", "build", "node_modules", ".git", "artifacts", "dist", ".claude"}
LINK_RE = re.compile(r"!?\[[^\]]*\]\(\s*(<[^>]+>|[^)\s]+)(?:\s+\"[^\"]*\")?\s*\)")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
CODE_FENCE = re.compile(r"^\s*(```|~~~)")


def slug(text: str) -> str:
    text = re.sub(r"`([^`]*)`", r"\1", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = text.strip().lower()
    text = re.sub(r"[^\w\- ]", "", text, flags=re.UNICODE)
    return text.replace(" ", "-")


def anchors_of(path: Path, cache: dict[Path, set[str]]) -> set[str]:
    if path in cache:
        return cache[path]
    seen: dict[str, int] = {}
    out: set[str] = set()
    in_code = False
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if CODE_FENCE.match(line):
            in_code = not in_code
            continue
        if in_code:
            continue
        m = HEADING_RE.match(line)
        if m:
            s = slug(m.group(2))
            n = seen.get(s, 0)
            seen[s] = n + 1
            out.add(s if n == 0 else f"{s}-{n}")
    cache[path] = out
    return out


def markdown_files(include_evidence: bool) -> list[Path]:
    files = []
    for p in ROOT.rglob("*.md"):
        rel = p.relative_to(ROOT)
        if any(part in SKIP_DIRS for part in rel.parts):
            continue
        if not include_evidence and rel.parts[:3] == ("docs", "test-evidence"):
            continue
        files.append(p)
    return sorted(files)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--include-evidence", action="store_true", help="also check docs/test-evidence/**")
    args = ap.parse_args()
    cache: dict[Path, set[str]] = {}
    broken: list[str] = []
    checked = external = 0
    files = markdown_files(args.include_evidence)
    for md in files:
        in_code = False
        for lineno, line in enumerate(md.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if CODE_FENCE.match(line):
                in_code = not in_code
                continue
            if in_code:
                continue
            line = re.sub(r"`[^`]*`", "", line)  # inline code is not a link
            for m in LINK_RE.finditer(line):
                target = m.group(1).strip("<>")
                if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.I):
                    external += 1
                    continue
                checked += 1
                path_part, _, frag = target.partition("#")
                dest = md if not path_part else (md.parent / path_part.split("?")[0]).resolve()
                rel = md.relative_to(ROOT)
                if not dest.exists():
                    broken.append(f"{rel}:{lineno}: missing target '{target}'")
                    continue
                if frag and dest.suffix.lower() == ".md" and dest.is_file():
                    if frag.lower() not in anchors_of(dest, cache):
                        broken.append(f"{rel}:{lineno}: missing anchor '#{frag}' in {dest.relative_to(ROOT)}")
    for b in broken:
        print("BROKEN", b)
    print(f"{len(files)} markdown files, {checked} relative links checked, {external} external links not fetched, "
          f"{len(broken)} broken")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
