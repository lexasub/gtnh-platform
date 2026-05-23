#!/usr/bin/env bash
# build_items_db.sh — Convert items.csv to SQLite items.db
#
# Usage:
#   ./build_items_db.sh [--db-path PATH] [--csv-path PATH]
#
# Default output: data/registry/items.db
# Default input:  data/registry/items.csv

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

DB_PATH="${PROJECT_ROOT}/data/registry/items.db"
CSV_PATH="${PROJECT_ROOT}/data/registry/items.csv"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--db-path PATH] [--csv-path PATH]

Convert items.csv to SQLite items.db.

Arguments:
  --db-path PATH   Path to output SQLite database (default: data/registry/items.db)
  --csv-path PATH  Path to input CSV file (default: data/registry/items.csv)

Example:
  $(basename "$0")
  $(basename "$0") --db-path /tmp/items.db --csv-path /path/to/items.csv
EOF
    exit 1
}

# Parse options
while [[ $# -gt 0 ]]; do
    case "$1" in
        --db-path)
            DB_PATH="$2"
            shift 2
            ;;
        --csv-path)
            CSV_PATH="$2"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            ;;
    esac
done

# Validate inputs
if [[ ! -f "$CSV_PATH" ]]; then
    echo "Error: CSV file not found: $CSV_PATH" >&2
    exit 1
fi

mkdir -p "$(dirname "$DB_PATH")"

echo "Converting items.csv → items.db"
echo "  Input: $CSV_PATH"
echo "  Output: $DB_PATH"
echo

# Remove existing DB for idempotency
rm -f "$DB_PATH"

# Create database schema
sqlite3 "$DB_PATH" <<'EOF'
CREATE TABLE IF NOT EXISTS items (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL UNIQUE,
    stack_size  INTEGER DEFAULT 64,
    meta        INTEGER DEFAULT 0,
    display_name TEXT
);
EOF

# Import CSV, skipping header
sqlite3 "$DB_PATH" <<EOF
.mode csv
.import --skip 1 "$CSV_PATH" items
EOF

# Verify import
COUNT=$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM items;")
echo
echo "✓ Successfully imported $COUNT items into $DB_PATH"
