#!/usr/bin/env bash
# E2E integration test: MessageRouter + MetaDB + FlatBuffers protocol
#
# Usage: scripts/e2e-test.sh [--build]
#   --build  Build binaries first

set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="$(pwd)/build"
META_DIR="$(pwd)/src/services/meta_db"
ROUTER_DIR="$(pwd)/src/services/message_router"
SCRIPT_DIR="$(pwd)/scripts"

if [ "${1:-}" = "--build" ]; then
  echo "=== Building MessageRouter ==="
  #cd "$ROUTER_DIR" && go build -o "$BUILD_DIR/src/services/message_router/message_router" .

  echo "=== Building MetaDB ==="
  cd "$META_DIR" && go build -o "$BUILD_DIR/src/services/meta_db/metadbd" .

  echo "=== Regenerating cmake ==="
  mkdir -p "$BUILD_DIR"
  cd "$BUILD_DIR" && cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1
fi

cleanup() {
  echo "=== Cleaning up ==="
  [ -n "${ROUTER_PID:-}" ] && kill "$ROUTER_PID" 2>/dev/null || true
  [ -n "${METADB_PID:-}" ] && kill "$METADB_PID" 2>/dev/null || true
  rm -f "$META_DIR/metadb.sqlite"
  wait 2>/dev/null
}
trap cleanup EXIT

echo "=== Starting MessageRouter on :4000 ==="
"$BUILD_DIR/src/services/message_router/message_router" &
ROUTER_PID=$!
sleep 1

echo "=== Starting MetaDB (JSON :5005, FB :5006) ==="
cd "$META_DIR"
"$BUILD_DIR/src/services/meta_db/metadbd" &
METADB_PID=$!
sleep 1

echo "=== Running E2E tests ==="
cd "$META_DIR"
go test -tags=e2e -v -run TestMetaDBE2E -timeout 30s
