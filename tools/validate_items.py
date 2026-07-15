#!/usr/bin/env python3
import sys
from pathlib import Path
from collections import defaultdict

from editor_model import parse_csv, validate_ids


def validate_prefix_free(groups: dict[str, list]) -> list[str]:
    errors = []
    prefixes = sorted(groups.keys())
    for i, p1 in enumerate(prefixes):
        for p2 in prefixes[i+1:]:
            if p2.startswith(p1 + ":") or p1.startswith(p2 + ":"):
                errors.append(f"Prefix collision: {p1} is prefix of {p2}")
    return errors


def main():
    csv_path = sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).parent.parent / "data" / "registry" / "items.csv")
    if not Path(csv_path).exists():
        print(f"File not found: {csv_path}")
        sys.exit(1)

    items, grouped, labels = parse_csv(csv_path)
    print(f"Parsed {len(items)} items, {len(grouped)} groups")

    all_errors = []

    print("\n[1] Duplicate ID check...")
    dup_errors = validate_ids(items)
    all_errors.extend(dup_errors)
    if not dup_errors:
        print("  ✓ No duplicate IDs")

    print("\n[2] Prefix-free property check...")
    pf_errors = validate_prefix_free(grouped)
    all_errors.extend(pf_errors)
    if not pf_errors:
        print("  ✓ Prefix-free: no group is prefix of another")

    print(f"\n{'='*50}")
    if all_errors:
        print(f"FAILED: {len(all_errors)} error(s)")
        for e in all_errors:
            print(f"  ✗ {e}")
        sys.exit(1)
    else:
        print(f"PASSED: {len(items)} items, {len(grouped)} groups")
        sys.exit(0)


if __name__ == "__main__":
    main()
