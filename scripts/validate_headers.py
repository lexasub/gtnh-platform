#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from _lib import ROOT, read, rel, run, tracked

USING_NS_STD = re.compile(r"\busing\s+namespace\s+std\b")
IFNDEF_GUARD = re.compile(r"#ifndef\s+(\w+)\s*\n\s*#define\s+\1\b")


def check_pragma_once() -> list[str]:
    out: list[str] = []
    for f in tracked("src/**/*.h", "src/**/*.hpp"):
        text = read(f)
        if "generated/" in rel(f):
            continue
        if "#pragma once" in text:
            continue
        if IFNDEF_GUARD.search(text):
            out.append(f"{rel(f)}: uses #ifndef guard instead of #pragma once (inconsistent)")
    return out


def check_no_using_namespace_std() -> list[str]:
    out: list[str] = []
    for f in tracked("src/**/*.h", "src/**/*.hpp"):
        if "generated/" in rel(f):
            continue
        text = read(f)
        for n, line in enumerate(text.splitlines(), 1):
            if USING_NS_STD.search(line) and not line.strip().startswith("//"):
                out.append(f"{rel(f)}:{n}: 'using namespace std' in header")
    return out


def main() -> int:
    return run([
        ("headers use #pragma once", check_pragma_once),
        ("no 'using namespace std' in headers", check_no_using_namespace_std),
    ])


if __name__ == "__main__":
    raise SystemExit(main())
