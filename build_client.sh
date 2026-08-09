#!/usr/bin/env bash
#
# build_client.sh — Portable client builder for GTNH Platform
#
# Usage:
#   ./build_client.sh           # full build (conan → cmake → shaders → package)
#   ./build_client.sh --quick   # skip conan, rebuild only changed files
#   ./build_client.sh --help    # show this help
#
# Output: build/gtnh-client/  — portable bundle (binary + shaders)
# Run with: ./gtnh-client/gameclientd
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
SHADER_SRC="${ROOT_DIR}/shaders"
BGFX_DIR="${ROOT_DIR}/third_party/bgfx.cmake"
SHADERC_PATH="${BGFX_DIR}/build/cmake/bgfx/shaderc"
CONAN_DIR="${ROOT_DIR}/conan"
BUNDLE_DIR="${BUILD_DIR}/gtnh-client"
JOBS=$(nproc)

# ── Colors ──────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC}  $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }
header(){ echo -e "\n${CYAN}━━━ $1 ━━━${NC}"; }

# ── Help ────────────────────────────────────────────────────────
if [[ "${1:-}" == "--help" ]]; then
    sed -n '2,11p' "$0"
    exit 0
fi

QUICK=false
if [[ "${1:-}" == "--quick" ]]; then QUICK=true; fi

# ── Prerequisites ───────────────────────────────────────────────
header "Checking prerequisites"
command -v cmake   >/dev/null 2>&1 || error "cmake is required (apt install cmake)"
if ! $QUICK; then
    command -v conan >/dev/null 2>&1 || error "conan is required (pip install conan)"
fi
info "cmake: $(cmake --version | head -1)"
if ! $QUICK; then
    info "conan: $(conan --version)"
fi

# ── Step 1: Conan dependencies ──────────────────────────────────
if ! $QUICK; then
    header "Installing conan dependencies"
    if [ ! -f "${CONAN_DIR}/conan_toolchain.cmake" ]; then
        info "Running conan install..."
        conan install "${ROOT_DIR}" \
            --output-folder="${CONAN_DIR}" \
            --build=missing
        info "Conan dependencies installed"
    else
        info "Conan dependencies already installed (${CONAN_DIR}/conan_toolchain.cmake)"
    fi
fi

# ── Step 2: Build shaderc if missing ────────────────────────────
if [ ! -f "$SHADERC_PATH" ]; then
    header "Building shaderc (bgfx shader compiler)"
    info "shaderc not found at ${SHADERC_PATH}, building bgfx.cmake..."
    cmake -S "${BGFX_DIR}" \
          -B "${BGFX_DIR}/build" \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_CXX_STANDARD=17
    cmake --build "${BGFX_DIR}/build" --target shaderc -j"$JOBS"
    if [ ! -f "$SHADERC_PATH" ]; then
        error "shaderc build failed — see errors above"
    fi
    info "shaderc built successfully"
else
    info "shaderc found at ${SHADERC_PATH}"
fi

# ── Step 3: Compile shaders ────────────────────────────────────
header "Compiling shaders"
SHADER_OUT="${BUILD_DIR}/shaders"
mkdir -p "$SHADER_OUT"

compile_shader() {
    local src=$1 variant=$2 profile=$3
    local name
    name=$(basename "$src" .sc)
    local out="${SHADER_OUT}/${name}.${variant}.bin"
    info "  ${src} → ${out}"
    "$SHADERC_PATH" \
        -f "$src" \
        -o "$out" \
        --platform linux \
        --type "$variant" \
        --profile "$profile" \
        -i "$SHADER_SRC" \
        -i "${BGFX_DIR}/bgfx/src" \
        --verbose
}

compile_shader "${SHADER_SRC}/vs_block.sc" "vert" "120"
compile_shader "${SHADER_SRC}/fs_block.sc" "frag" "120"
compile_shader "${SHADER_SRC}/vs_imgui.sc" "vert" "120"
compile_shader "${SHADER_SRC}/fs_imgui.sc" "frag" "120"
info "All shaders compiled"

# ── Step 4: Build the client binary ─────────────────────────────
header "Building client binary (gameclientd)"
if [ ! -f "${BUILD_DIR}/CMakeCache.txt" ]; then
    info "Configuring cmake..."
    cmake -S "${ROOT_DIR}" \
          -B "${BUILD_DIR}" \
          -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "${BUILD_DIR}" --target gameclientd -j"$JOBS"
info "Client built: ${BUILD_DIR}/bin/gameclientd"

# ── Step 5: Package portable bundle ─────────────────────────────
header "Packaging portable bundle"
rm -rf "$BUNDLE_DIR"
mkdir -p "${BUNDLE_DIR}/shaders"

cp "${BUILD_DIR}/bin/gameclientd" "${BUNDLE_DIR}/"
cp "${SHADER_OUT}/"*.bin "${BUNDLE_DIR}/shaders/"

echo ""
info "========================================"
info "  Portable client built!"
info "  Bundle: ${BUNDLE_DIR}"
info "========================================"
echo ""
echo -e "  ${GREEN}gameclientd${NC}   — client binary (${CYAN}$(du -h "${BUNDLE_DIR}/gameclientd" | cut -f1)${NC})"
echo -e "  ${GREEN}shaders/${NC}      — compiled shaders (${CYAN}$(du -h "${BUNDLE_DIR}/shaders" | cut -f1)${NC})"
echo ""
echo -e "  ${YELLOW}Run:${NC}"
echo -e "    cd ${BUNDLE_DIR}"
echo -e "    ./gameclientd"
echo ""
echo -e "  ${YELLOW}With custom shader dir:${NC}"
echo -e "    ./gameclientd --shader-dir /path/to/shaders"
echo ""
echo -e "  ${YELLOW}Runtime dependencies (must be installed):${NC}"
echo -e "    liburing, libGL, libtbb, X11 libraries"
echo ""
