#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from _lib import ROOT, read, rel, run, tracked

ENV_PORT_RE = re.compile(r"(\w+_PORT)\s*=\s*(\d+)")

NETWORK_PORT_NAMES = re.compile(
    r"""\b(?:router_port|ctrl_port|bulk_port|tcp_port|fbPort|rpc_port|json_port|port)\s*=\s*(\d{4,5})""",
    re.IGNORECASE,
)
GO_ADDR_PORT_RE = re.compile(r'":(\d{4,5})"')
GO_PORT_VAR_RE = re.compile(r'\bport\s*=\s*":(\d{4,5})"')


def _env_ports() -> dict[str, str]:
    f = ROOT / ".env.example"
    if not f.exists():
        return {}
    return {m.group(1): m.group(2) for m in ENV_PORT_RE.finditer(read(f))}


def _code_ports() -> dict[str, list[str]]:
    by_port: dict[str, list[str]] = {}
    for f in tracked("src/**/*.cpp", "src/**/*.h", "src/**/*.go"):
        if "/test" in rel(f) or "_test." in f.name:
            continue
        text = read(f)
        for m in NETWORK_PORT_NAMES.finditer(text):
            by_port.setdefault(m.group(1), []).append(f"{rel(f)}:{m.start()}")
        for m in GO_ADDR_PORT_RE.finditer(text):
            by_port.setdefault(m.group(1), []).append(f"{rel(f)}:{m.start()}")
        for m in GO_PORT_VAR_RE.finditer(text):
            by_port.setdefault(m.group(1), []).append(f"{rel(f)}:{m.start()}")
    return by_port


def check_env_ports_documented() -> list[str]:
    out: list[str] = []
    env = _env_ports()
    code = _code_ports()
    env_values = set(env.values())
    code_ports = set(code.keys())

    for port in sorted(code_ports - env_values):
        locs = code[port][:3]
        out.append(f"port {port} in code but not in .env.example ({', '.join(locs)})")
    return out


def main() -> int:
    return run([
        ("all code ports documented in .env.example", check_env_ports_documented),
    ])


if __name__ == "__main__":
    raise SystemExit(main())
