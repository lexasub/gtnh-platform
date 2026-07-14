#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from _lib import ROOT, read, rel, run, tracked


def _check_strict_mode(content: str) -> list[str]:
    lines = content.splitlines()
    effective_lines = [l for l in lines if l.strip() and not l.strip().startswith("#")]
    if not effective_lines:
        return []

    has_pipefail = any("pipefail" in l for l in effective_lines[:5])
    has_set_e = any(re.match(r"set\s+-[a-zA-Z]*e", l) for l in effective_lines[:5])

    problems = []
    if not has_set_e:
        problems.append("missing 'set -e' in first effective lines")
    if not has_pipefail:
        problems.append("missing 'pipefail' in first effective lines")
    return problems


def check_shell_strict_mode() -> list[str]:
    out: list[str] = []
    for f in tracked("*.sh", "scripts/*.sh", "ci/*.sh"):
        if "bgfx_shader" in f.name:
            continue
        content = read(f)
        if "# @no-strict" in content:
            continue
        problems = _check_strict_mode(content)
        for p in problems:
            out.append(f"{rel(f)}: {p}")
    return out


def main() -> int:
    return run([
        ("shell scripts have strict mode (set -euo pipefail)", check_shell_strict_mode),
    ])


if __name__ == "__main__":
    raise SystemExit(main())
