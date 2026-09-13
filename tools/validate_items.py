#!/usr/bin/env python3
import sys
from pathlib import Path

try:
    from .editor_model import parse_csv, validate_items_strict
except ImportError:  # direct script invocation from the repository root
    from editor_model import parse_csv, validate_items_strict


def main() -> int:
    csv_path = sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).parent.parent / "data" / "registry" / "items.csv")
    if not Path(csv_path).exists():
        print(f"File not found: {csv_path}")
        return 1

    items, grouped, _ = parse_csv(csv_path)
    print(f"Parsed {len(items)} items, {len(grouped)} groups")
    print("\n[1] Strict item ID check...")
    errors = validate_items_strict(csv_path)
    if not errors:
        print("  ✓ No malformed, overflowing, or duplicate packed IDs")
    else:
        for error in errors:
            print(f"  ✗ {error}")

    print(f"\n{'=' * 50}")
    if errors:
        print(f"FAILED: {len(errors)} error(s)")
        return 1
    print(f"PASSED: {len(items)} items, {len(grouped)} groups")
    return 0


if __name__ == "__main__":
    sys.exit(main())
