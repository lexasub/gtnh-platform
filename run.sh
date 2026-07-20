#!/usr/bin/env bash
set -euo pipefail

# GTNH Platform — start all services and game client
# Usage: ./run.sh [--build-dir <path>] [--db-dir <path>] [--resolution <WxH>] [--all] [--no-client]
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/cmake-build-debug"
DB_DIR="${SCRIPT_DIR}/chunkdb"
START_ALL=false
START_CLIENT=false 
#true
RESOLUTION="2000x1200"

cd cmake-build-debug; ninja -j5; cd ..
cp -r data/ /mnt/nfs/src/cpp/gtnh-platform/
sudo cp "${BUILD_DIR}"/bin/gameclientd /mnt/nfs/
pushd ${SCRIPT_DIR}/src/services/message_router/
go build *.go
popd
cp ${SCRIPT_DIR}/src/services/message_router/message_router $BUILD_DIR/src/services/message_router/routerd
pushd ${SCRIPT_DIR}/src/services/meta_db/ > /dev/null
go build -o metadbd *.go
popd > /dev/null
echo "  → MetaDB rebuilt"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)  BUILD_DIR="$2";  shift 2 ;;
        --db-dir)     DB_DIR="$2";     shift 2 ;;
        --resolution) RESOLUTION="$2"; shift 2 ;;
        --all)        START_ALL=true;  shift   ;;
        --no-client)  START_CLIENT=false; shift ;;
        --help|-h)
            echo "Usage: $0 [--build-dir <path>] [--db-dir <path>] [--resolution <WxH>] [--all] [--no-client]"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

BIN() { echo "${BUILD_DIR}/src/services/$1/$2"; }

ROUTERD="$(BIN message_router routerd)"
ROUTER_SRC="${SCRIPT_DIR}/src/services/message_router"
CHUNKD="$(BIN chunk_store chunkd)"
GATEWAYD="$(BIN gateway gatewayd)"
SIMCORED="$(BIN simulation_core simcored_exec)"
CLIENT="${BUILD_DIR}/bin/gameclientd"
PIPENETWORKD="$(BIN pipe_network pipenetworkd)"
SPATIALINDEXD="$(BIN spatial_index spatialindexd)"
METADBD="${SCRIPT_DIR}/src/services/meta_db/metadbd"
ENTITYSTATED="$(BIN entity_state_store entitystated)"
VALIDATIOND="$(BIN validation validationd)"

# ── preamble ──────────────────────────────────────────────────────

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'; CYAN='\033[0;36m'; NC='\033[0m'
ok()   { printf "  ${GREEN}✓${NC} %s\n" "$1"; }
info() { printf "  ${CYAN}→${NC} %s\n" "$1"; }
warn() { printf "  ${YELLOW}⚠${NC} %s\n" "$1"; }
die()  { printf "  ${RED}✗${NC} %s\n" "$1"; exit 1; }

# ── build Go routerd if needed ────────────────────────────────────

# routerd from CMake is a C++ stub that immediately exits;
# rebuild it as a real Go binary if it's still the stub (<100KB).
if [ -f "${ROUTERD}" ] && [ "$(stat -c%s "${ROUTERD}" 2>/dev/null)" -lt 100000 ]; then
    info "routerd is a C++ stub — rebuilding from Go sources …"
    if command -v go &>/dev/null; then
        (cd "${ROUTER_SRC}" && go build -o "${ROUTERD}" main.go router.go) || die "go build failed"
        ok "routerd rebuilt ($(stat -c%s "${ROUTERD}") bytes)"
    else
        die "Go toolchain not found — install go or build routerd manually"
    fi
fi

# ── check prerequisites ───────────────────────────────────────────

for exe in "${ROUTERD}" "${CHUNKD}" "${GATEWAYD}" "${SIMCORED}" "${ENTITYSTATED}" "${METADBD}"; do
    [ -x "$exe" ] || die "Binary not found: ${exe} — did you run make?"
done

if $START_CLIENT; then
    [ -x "${CLIENT}" ] || die "Binary not found: ${CLIENT} — did you run make?"
fi

if $START_ALL; then
    for exe in "${PIPENETWORKD}" "${SPATIALINDEXD}" "${VALIDATIOND}"; do
        [ -x "$exe" ] || die "Binary not found: ${exe} — did you run make?"
    done
fi

# ── ensure LMDB directory exists ──────────────────────────────────

mkdir -p "${DB_DIR}"
ok "DB directory: ${DB_DIR}"

# ── kill leftover GTNH processes ────────────────────────────

info "Cleaning up any leftover processes …"
for proc in routerd chunkd gatewayd simcored_exec gameclientd entitystated pipenetworkd spatialindexd metadbd validationd; do
    pids=$(pgrep -x "$proc" 2>/dev/null || true)
    if [ -n "$pids" ]; then
        warn "Killing leftover $proc (PID $pids)"
        kill $pids 2>/dev/null || true
    fi
done
sleep 0.5
# force-kill any survivors
for proc in routerd chunkd gatewayd simcored_exec gameclientd entitystated; do
    pids=$(pgrep -x "$proc" 2>/dev/null || true)
    [ -n "$pids" ] && kill -9 $pids 2>/dev/null || true
done
sleep 0.3
ok "Clean"

# ── process management ────────────────────────────────────────────

PID_FILE=$(mktemp /tmp/gtnh-pids.XXXXXX)
trap 'cleanup' INT TERM

cleanup() {
    echo ""
    info "Shutting down all services …"
    if [ -f "$PID_FILE" ]; then
        # reverse order: client first, router last
        tac "$PID_FILE" | while read -r pid; do
            kill "$pid" 2>/dev/null || true
        done
        rm -f "$PID_FILE"
    fi
    ok "All services stopped."
    exit 0
}

LAUNCH() {
    local name="$1" bin="$2" log; shift 2
    log="/tmp/gtnh-${name}.log"
    info "Starting ${name} …"
    : > "${log}"
    GTNH_LOG_LEVEL=TRACE "$bin" "$@" &> /dev/stdout | tee "${log}" &
    local pid=$!
    echo "$pid" >> "$PID_FILE"
    ok "${name} (PID ${pid}) — log: ${log}"
    sleep 0.5
    # quick check: did it already die?
    if ! kill -0 "$pid" 2>/dev/null; then
        warn "${name} exited immediately — check ${log}"
    fi
}

# ── start services ────────────────────────────────────────────────

printf "\n${CYAN}═══ GTNH Platform ═══${NC}\n\n"

LAUNCH "routerd"        "${ROUTERD}"
LAUNCH "chunkd"         "${CHUNKD}"         "${DB_DIR}"  5001  "127.0.0.1"  4000
LAUNCH "gatewayd"       "${GATEWAYD}"       --router-port 4000  --port 7777
LAUNCH "entitystated"   "${ENTITYSTATED}"
LAUNCH "simcored"       "${SIMCORED}"       "127.0.0.1"  4000  "127.0.0.1"  5001 /home/su/src/local/gtnh-platform/data/recipes
LAUNCH "metadbd"        "${METADBD}"

if $START_ALL; then
    LAUNCH "pipenetworkd"  "${PIPENETWORKD}"
    LAUNCH "spatialindexd" "${SPATIALINDEXD}"
    LAUNCH "validationd"   "${VALIDATIOND}"
fi

if $START_CLIENT; then
    printf "\n${CYAN}═══ Starting client in 2 seconds … ═══${NC}\n\n"
    sleep 2
    if [ -n "$RESOLUTION" ]; then
        LAUNCH "gameclientd" "${CLIENT}" --resolution "${RESOLUTION}"
    else
        LAUNCH "gameclientd" "${CLIENT}" --resolution 2000x1200
    fi
fi
#rsync -a src/ /mnt/nfs/src/cpp/gtnh-platform/src --exclude 'CMakeLists.txt'
printf "\n${GREEN}All services running. Press Ctrl+C to stop.${NC}\n"
#cd test/loadtest
#./loadtest -ctrl 127.0.0.1:7777 -rate 50 -duration 10
# Block until user sends SIGINT/SIGTERM
wait
