#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from _lib import ROOT, read, rel, run, tracked


def _conan_packages() -> set[str]:
    f = ROOT / "conanfile.txt"
    if not f.exists():
        return set()
    packages = set()
    in_requires = False
    for line in read(f).splitlines():
        stripped = line.strip()
        if stripped == "[requires]":
            in_requires = True
            continue
        if stripped.startswith("[") and in_requires:
            in_requires = False
            continue
        if in_requires and stripped and not stripped.startswith("#"):
            name = stripped.split("/")[0].split("[")[0].strip()
            if name:
                packages.add(name.lower())
    return packages


def _cmake_packages() -> set[str]:
    packages = set()
    for f in tracked("CMakeLists.txt", "src/**/CMakeLists.txt"):
        for m in re.finditer(r"find_package\((\w+)", read(f)):
            name = m.group(1).lower()
            if name not in ("gtest", "bgfx", "boost", "tbb", "threads", "xorg", "opengl"):
                packages.add(name)
    return packages


def check_cmake_has_conan() -> list[str]:
    out: list[str] = []
    conan = _conan_packages()
    cmake = _cmake_packages()
    alias = {"glfw3": "glfw", "nlohmann_json": "nlohmann_json", "fastnoise2": "fastnoise2", "yaml": "yaml-cpp"}

    for pkg in sorted(cmake):
        mapped = alias.get(pkg, pkg)
        if mapped not in conan:
            out.append(f"find_package({pkg}) has no conanfile.txt entry")
    return out


def main() -> int:
    return run([
        ("every find_package has a conanfile.txt entry", check_cmake_has_conan),
    ])


if __name__ == "__main__":
    raise SystemExit(main())
