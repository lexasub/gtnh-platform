#!/usr/bin/env python3
"""Meta-guard (anti-drift guard-catalog L): CI/pre-commit/gate parity.

Asserts, deterministically and offline, the pillars that otherwise drift silently
in prose and comments:
  1. every scripts/validate_*.py is wired into .pre-commit-config.yaml as an entry
     (so local commits run the same guards as the gate and CI — not a subset);
  2. every scripts/validate_*.py has a sibling scripts/test_validate_*.py
     (guard-the-guard: a guard without a unit test that proves it catches drift
      is unverified);
  3. every CI workflow that re-implements checks (ctest / cmake --build /
     python3 scripts/validate_*) delegates to the in-repo gate (ci/check.sh)
     instead — the "logic in CI-YAML" anti-pattern is a parity violation.

Self-included: this validator is itself a validate_*.py, so rules 1–2 apply to it.
"""
from __future__ import annotations

import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from _lib import ROOT, read, run, tracked

PRE_COMMIT = ROOT / ".pre-commit-config.yaml"
WORKFLOWS = sorted((ROOT / ".github" / "workflows").glob("*.yml")) if (ROOT / ".github" / "workflows").is_dir() else []

# Steps that only make sense delegated to the gate; presence without ci/check.sh = reimplementation.
REIMPL_RE = re.compile(r"\bctest\b|cmake\s+--build|python3 scripts/validate_")


def _validator_names() -> list[str]:
    return [p.name for p in tracked("scripts/validate_*.py")]


def check_precommit_parity() -> list[str]:
    out: list[str] = []
    if not PRE_COMMIT.exists():
        return ["missing .pre-commit-config.yaml"]
    cfg = read(PRE_COMMIT)
    for name in _validator_names():
        if name not in cfg:
            out.append(f"{name} not wired into .pre-commit-config.yaml")
    for m in re.finditer(r"python3 (scripts/validate_\w+\.py)", cfg):
        if not (ROOT / m.group(1)).exists():
            out.append(f"pre-commit references missing file {m.group(1)}")
    return out


def check_guard_tests() -> list[str]:
    out: list[str] = []
    for name in _validator_names():
        test = name.replace("validate_", "test_validate_", 1)
        if not (ROOT / "scripts" / test).exists():
            out.append(f"{name} has no sibling unit test {test}")
    return out


def check_ci_delegates_to_gate() -> list[str]:
    out: list[str] = []
    for wf in WORKFLOWS:
        body = read(wf)
        if REIMPL_RE.search(body) and "ci/check.sh" not in body:
            out.append(f"{wf.name} re-implements checks (ctest/cmake --build/validate_*) "
                       "without delegating to ci/check.sh")
    return out


def main() -> int:
    return run([
        ("every validate_*.py wired into pre-commit", check_precommit_parity),
        ("every validate_*.py has a unit test", check_guard_tests),
        ("CI workflows delegate to the gate (ci/check.sh)", check_ci_delegates_to_gate),
    ])


if __name__ == "__main__":
    raise SystemExit(main())
