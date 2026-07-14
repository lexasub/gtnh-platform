#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from _lib import ROOT, iter_code_cstyle, read, rel, run, tracked

FORBIDDEN = re.compile(r"\bconsole\.log\b|\beval\s*\(")


def check_forbidden_tokens() -> list[str]:
    out: list[str] = []
    globs = (
        "src/**/*.cpp", "src/**/*.hpp", "src/**/*.h",
        "src/**/*.go",
    )
    for f in tracked(*globs):
        for n, code in iter_code_cstyle(read(f)):
            m = FORBIDDEN.search(code)
            if m:
                out.append(f"{rel(f)}:{n}: forbidden token {m.group(0)!r}")
    return out


def main() -> int:
    return run([
        ("no forbidden tokens in source code", check_forbidden_tokens),
    ])


if __name__ == "__main__":
    raise SystemExit(main())
