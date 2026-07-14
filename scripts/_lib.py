"""Shared helpers for deterministic anti-drift validators.

All validators built on this are stdlib-only, offline, fail-closed, fast, and run identically
from pre-commit, the in-repo gate (ci/check.sh tooling), and CI. Import with:

    import sys, pathlib
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    from _lib import ROOT, tracked, iter_code, read, run

so it works when launched as `python3 scripts/validate_x.py` from any cwd.
"""

from __future__ import annotations

import re
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent  # adjust if scripts/ is nested differently

# Dirs the no-git fallback in _all_files() prunes.
_FALLBACK_PRUNE = {".git", "node_modules", ".venv", "venv", "__pycache__", "dist", "build",
                   ".mypy_cache", ".pytest_cache", ".ruff_cache", ".tox", ".next", "target"}


def _glob_to_re(glob: str) -> re.Pattern[str]:
    """Glob -> regex where `*` matches across `/` (git-pathspec style)."""
    out = []
    for ch in glob:
        out.append("[^\0]*" if ch == "*" else "." if ch == "?" else re.escape(ch))
    return re.compile("^" + "".join(out) + "$")


def _all_files() -> list[Path]:
    try:
        proc = subprocess.run(
            ["git", "-C", str(ROOT), "ls-files", "-z", "--cached", "--others",
             "--exclude-standard"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
        )
        if proc.returncode == 0:
            return [ROOT / n for n in proc.stdout.split("\0") if n]
    except OSError:
        pass
    return [p for p in ROOT.rglob("*") if not _FALLBACK_PRUNE & set(p.relative_to(ROOT).parts)]


def rel(p: Path) -> str:
    try:
        return str(p.resolve().relative_to(ROOT.resolve()))
    except ValueError:
        return str(p)


def tracked(*globs: str) -> list[Path]:
    """Tracked + not-ignored files matching any glob; regular files only."""
    pats = [_glob_to_re(g) for g in globs]
    out = [p for p in _all_files()
           if p.is_file() and not p.is_symlink() and any(pat.match(rel(p)) for pat in pats)]
    return sorted(set(out))


def read(p: Path) -> str:
    return p.read_text(encoding="utf-8-sig")


def iter_code(text: str):
    """Yield (lineno, code) for source with triple-quoted strings blanked and inline comments stripped."""
    def _blank(m: re.Match[str]) -> str:
        return re.sub(r"[^\n]", " ", m.group(0))
    t = re.sub(r'""".*?"""', _blank, text, flags=re.S)
    t = re.sub(r"'''.*?'''", _blank, t, flags=re.S)
    for i, line in enumerate(t.splitlines(), 1):
        yield i, line.split("#", 1)[0]


def iter_code_cstyle(text: str):
    """iter_code() for C-style sources (C/C++/Go/JS/TS): line/block comments blanked, string literals kept."""
    res: list[str] = []
    i, n = 0, len(text)
    quote = None
    block = line_comment = False
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if line_comment:
            if c == "\n":
                line_comment = False
                res.append(c)
            else:
                res.append(" ")
            i += 1
        elif block:
            if c == "*" and nxt == "/":
                res.append("  "); i += 2; block = False
            else:
                res.append(c if c == "\n" else " "); i += 1
        elif quote is not None:
            res.append(c)
            if c == "\\" and nxt:
                res.append(nxt); i += 2
            else:
                if c == quote:
                    quote = None
                i += 1
        elif c == "/" and nxt == "/":
            line_comment = True; res.append("  "); i += 2
        elif c == "/" and nxt == "*":
            block = True; res.append("  "); i += 2
        elif c in "\"'`":
            quote = c; res.append(c); i += 1
        else:
            res.append(c); i += 1
    for idx, line in enumerate("".join(res).splitlines(), 1):
        yield idx, line


def run(checks: list[tuple[str, Callable[[], list[str]]]]) -> int:
    """Run named checks; print a line each; return 1 if any produced errors (fail-closed)."""
    total = 0
    for name, fn in checks:
        try:
            problems = fn()
        except Exception as e:
            problems = [f"validator crashed: {e!r}"]
        if problems:
            total += len(problems)
            print(f"FAIL {name} ({len(problems)})")
            for p in problems:
                print(f"  - {p}")
        else:
            print(f"ok   {name}")
    if total:
        print(f"\n{total} problem(s) found.", file=sys.stderr)
    return 1 if total else 0
