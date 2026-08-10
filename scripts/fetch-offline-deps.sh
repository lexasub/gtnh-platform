#!/bin/bash
set -euo pipefail

# fetch-offline-deps.sh — pull the offline deps bundle from GitHub Releases and
# lay it out for a Conan-free build.
#
# Used in CI/build pipelines: instead of running conan install + building
# bgfx from source (needs the local-only 58f999f pin), download the pre-built
# bundle from the release, unpack it, and point CMake at it.
#
# The bundle changes rarely — the download URL is hardcoded (deps release).
# Override with DEPS_URL env var when a new bundle is published.
#
# Layout produced (mirrors what make-offline-bundle.sh packs):
#   <dest>/deps/conan-offline/   Conan toolchain + configs
#   <dest>/deps/usr-local/       bgfx/bx/bimg static libs + headers (installed)
#   <dest>/install-deps.sh       installer for bgfx -> /usr/local
#
# Usage:
#   bash scripts/fetch-offline-deps.sh [dest_dir]
#     dest_dir  default: cmake-build-offline (gitignored)
#
# Requires: curl, tar

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${1:-cmake-build-offline}"
case "$DEST" in
    /*) DEST_DIR="$DEST" ;;              # absolute path
    *)  DEST_DIR="$ROOT/$DEST" ;;
esac
mkdir -p "$DEST_DIR"

# Hardcoded: deps live in the 'deps-0' release (lexasub/gtnh-platform).
# Rebuild & re-upload via scripts/make-offline-bundle.sh when libs change,
# then bump the tag/version here.
DEFAULT_URL="https://github.com/lexasub/gtnh-platform/releases/download/deps-0/gtnh-platform-deps-2026-08-10.tar.xz"
URL="${DEPS_URL:-$DEFAULT_URL}"

log() { printf '\033[1;32m[fetch-deps]\033[0m %s\n' "$*"; }

TARBALL="$DEST_DIR/$(basename "$URL")"

# ── 1. Download (skip if already present) ───────────────────────────────────
if [ -f "$TARBALL" ]; then
    log "tarball already present: $TARBALL (delete to force re-download)"
else
    log "downloading $URL"
    curl -fL --retry 3 -o "$TARBALL" "$URL"
fi

# ── 2. Unpack (no strip: bundle root = README + install-deps.sh + deps/) ────
log "unpacking to $DEST_DIR"
rm -rf "$DEST_DIR/deps" "$DEST_DIR/install-deps.sh" "$DEST_DIR/README-OFFLINE.md"
tar -xJf "$TARBALL" -C "$DEST_DIR"

# ── 3. Sanity ───────────────────────────────────────────────────────────────
[ -f "$DEST_DIR/deps/usr-local/lib/libbgfx.a" ] || {
    echo "bundle broken: no deps/usr-local/lib/libbgfx.a" >&2; exit 1; }
[ -f "$DEST_DIR/install-deps.sh" ] || {
    echo "bundle broken: no install-deps.sh" >&2; exit 1; }

log "done. build with (toolchain auto-detected from the bundle):"
log "  bash $DEST_DIR/install-deps.sh"
log "  cmake -B build && cmake --build build -j\$(nproc)"
