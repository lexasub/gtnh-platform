#!/bin/sh
# ci/check.sh — the in-repo gate. SINGLE source of truth for "is this repo OK".
#
# Run identically by: a developer locally, pre-commit hooks, and CI.
#
# Usage:
#   sh ci/check.sh            # everything (cpp + tooling)
#   sh ci/check.sh cpp        # cmake build + ctest
#   sh ci/check.sh tooling    # anti-drift guards + their unit tests
#
# Assumptions: deps installed (conan, cmake, ninja, g++).
# POSIX sh only. Fails loudly on the first error.

set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
TARGET=${1:-all}
log() { printf '\n=== %s ===\n' "$1"; }

# ── Detect build directory ────────────────────────────────────────
find_build_dir() {
    for d in cmake-build-debug cmake-build-release build; do
        if [ -f "$ROOT/$d/CMakeCache.txt" ]; then
            echo "$ROOT/$d"
            return
        fi
    done
    echo ""
}

# ── C++ build + test ──────────────────────────────────────────────
check_cpp() {
    BUILD_DIR=$(find_build_dir)
    if [ -z "$BUILD_DIR" ]; then
        echo "ERROR: no CMake build directory found (cmake-build-debug/ or cmake-build-release/)" >&2
        echo "Run: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build" >&2
        exit 1
    fi
    log "cpp: build ($(basename "$BUILD_DIR"))"
    cmake --build "$BUILD_DIR" -j "$(nproc)"
    log "cpp: test (ctest)"
    ( cd "$BUILD_DIR" && ctest --output-on-failure -j "$(nproc)" )
}

# ── Anti-drift guards ─────────────────────────────────────────────
FOUND=0
run_py_glob() {
    for f in $1; do
        [ -e "$f" ] || continue
        FOUND=$((FOUND + 1))
        printf '== %s\n' "$f"; ( cd "$ROOT" && python3 "$f" )
    done
}

check_tooling() {
    FOUND=0
    log "tooling: scripts/validate_*.py (anti-drift guards)"
    run_py_glob "$ROOT/scripts/validate_*.py"
    log "tooling: scripts/test_*.py (guard unit tests)"
    run_py_glob "$ROOT/scripts/test_*.py"
    [ "$FOUND" -gt 0 ] || {
        printf 'tooling: no scripts/validate_*.py or scripts/test_*.py found — vacuous target\n' >&2
        exit 1
    }
}

# ── Dispatch ──────────────────────────────────────────────────────
case "$TARGET" in
    cpp)     check_cpp ;;
    tooling) check_tooling ;;
    all)     check_cpp; check_tooling ;;
    *) printf 'Unknown target: %s (cpp | tooling | all)\n' "$TARGET" >&2; exit 2 ;;
esac

log "gate passed"
