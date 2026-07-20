#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from _lib import ROOT, read, rel, run, tracked

LINK_RE = re.compile(r"\[.*?\]\(([^)]+)\)")
EXTERNAL = re.compile(r"^(https?://|mailto:|#|@)")


def _in_code_block(lines: list[str], line_idx: int) -> bool:
    count = 0
    for i in range(line_idx):
        if lines[i].strip().startswith("```"):
            count += 1
    return count % 2 == 1


def check_doc_links() -> list[str]:
    out: list[str] = []
    for f in tracked("*.md", "doc/**/*.md", "openspec/**/*.md"):
        # Skip archived docs — broken links are expected
        if "/archive/" in str(f):
            continue
        text = read(f)
        fdir = f.parent
        lines = text.splitlines()
        for n, line in enumerate(lines, 1):
            if _in_code_block(lines, n - 1):
                continue
            for m in LINK_RE.finditer(line):
                target = m.group(1)
                if EXTERNAL.match(target):
                    continue
                target_clean = target.split("#")[0].split(" ")[0]
                if not target_clean:
                    continue
                resolved = (fdir / target_clean).resolve()
                if not resolved.exists():
                    out.append(f"{rel(f)}:{n}: broken link → {target_clean}")
    return out


def main() -> int:
    return run([
        ("relative links in markdown resolve to existing files", check_doc_links),
    ])


if __name__ == "__main__":
    raise SystemExit(main())
