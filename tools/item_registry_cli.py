#!/usr/bin/env python3
"""Read-only strict item registry validation and statistics CLI."""
from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path
from typing import Any, Sequence, Tuple

try:
    from .editor_model import (
        MIGRATION_POOLS,
        RegistryError,
        build_migration,
        parse_manifest,
        parse_csv,
        registry_stats,
        validate_items_strict,
        validate_manifest,
    )
except ImportError:  # direct script invocation from the repository root
    from editor_model import (
        MIGRATION_POOLS,
        RegistryError,
        build_migration,
        parse_manifest,
        parse_csv,
        registry_stats,
        validate_items_strict,
        validate_manifest,
    )

DEFAULT_MAP_PATH = "data/registry/item-id-migration.csv"

# Section markers in items.csv that bound the two migrated pool regions.
_POOL_A_BEGIN = "# base/processed — allocation pool 01110"
_POOL_A_END = "# base/tools — allocation pool 011110"
_POOL_B_BEGIN = "# machines — allocation pool 1110"
_POOL_B_END = "# infra — allocation pool 1111"


def _items(path: str):
    items, _, _ = parse_csv(path)
    return items


def _validate(args: argparse.Namespace) -> int:
    try:
        items = _items(args.items)
    except (OSError, UnicodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    errors = validate_items_strict(args.items)
    manifest = None
    if args.manifest:
        try:
            manifest = parse_manifest(args.manifest)
        except RegistryError as exc:
            errors.append(str(exc))
        else:
            errors.extend(validate_manifest(manifest, items))
    errors = sorted(set(errors))
    if args.json:
        print(json.dumps({"ok": not errors, "errors": errors}, sort_keys=True, separators=(",", ":")))
    else:
        if errors:
            for error in errors:
                print(f"ERROR: {error}")
        else:
            print(f"OK: {len(items)} items validated")
    return 1 if errors else 0


def _stats(args: argparse.Namespace) -> int:
    try:
        items = _items(args.items)
        manifest = parse_manifest(args.manifest)
        errors = validate_items_strict(args.items) + validate_manifest(manifest, items)
    except (OSError, UnicodeError, RegistryError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    if errors:
        errors = sorted(set(errors))
        if args.json:
            print(json.dumps({"ok": False, "errors": errors}, sort_keys=True, separators=(",", ":")))
        else:
            for error in errors:
                print(f"ERROR: {error}")
        return 1
    stats = registry_stats(items, manifest)
    if args.json:
        print(json.dumps(stats, sort_keys=True, separators=(",", ":")))
    else:
        for row in stats:
            free = ",".join(f"[{start},{end})" for start, end in row["free_ranges"]) or "none"
            print(
                f"{row['path']} prefix={row['prefix']} capacity={row['capacity']} "
                f"direct={row['direct_usage']} descendants={row['descendant_usage']} "
                f"child_reservations={row['child_reservations']} free={free} "
                f"headroom={row['headroom']['configured']}/{row['headroom']['absolute']}"
            )
    return 0


def _migrate(args: argparse.Namespace) -> int:
    try:
        items = _items(args.items)
        rows = build_migration(items)
    except (OSError, UnicodeError, RegistryError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    strict_errors = validate_items_strict(args.items)
    if strict_errors:
        for error in strict_errors:
            print(f"ERROR: {error}")
        print("ERROR: refusing to migrate while validation errors exist", file=sys.stderr)
        return 1
    if not args.apply:
        print(f"{'OLD ID':<16} {'NEW ID':<16} {'NAME':<28} {'SUBGROUP':<50} PACKED")
        for row in rows:
            if row.old_raw == row.new_raw:
                continue
            print(f"{row.old_raw:<16} {row.new_raw:<16} {row.name:<28} {row.subgroup:<50} "
                  f"{row.old_packed:#06x} -> {row.new_packed:#06x}")
        print(f"WARNING: this renumbers {sum(1 for r in rows if r.old_raw != r.new_raw)} item IDs; "
              "persistent world data (chunk_store LMDB chunks) still stores the old packed IDs. "
              "Run with --apply to rewrite items.csv and write the old→new map.")
        return 0
    try:
        _apply_migration(args.items, rows)
        _write_map(args.map, rows)
    except (OSError, RegistryError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(f"applied: rewrote {args.items} and wrote map to {args.map}")
    return 0


def _render_pool(rows: Sequence[Any], subgroups: Sequence[Tuple[str, str, Any]]) -> List[str]:
    lines: List[str] = []
    for subgroup, prefix, _ in subgroups:
        group_rows = [row for row in rows if row.subgroup == subgroup]
        suffix = "" if group_rows else " (reserved, empty)"
        lines.append(f"# {subgroup} — allocation prefix {prefix}{suffix}")
        for row in sorted(group_rows, key=lambda row: row.new_packed):
            lines.append(f"{row.new_raw},{row.name},{row.stack},{row.meta}")
    return lines


def _replace_region(lines: List[str], begin: str, end: str, block: List[str]) -> List[str]:
    try:
        start = lines.index(begin)
        stop = lines.index(end, start + 1)
    except ValueError:
        raise RegistryError(f"items.csv is missing pool section markers {begin!r} / {end!r}")
    return lines[:start + 1] + block + [""] + lines[stop:]


def _apply_migration(path: str, rows: Sequence[Any]) -> None:
    text = Path(path).read_text(encoding="utf-8")
    lines = text.splitlines()
    found = {line.split(",", 1)[0] for line in lines
             if line and not line.lstrip().startswith("#")}
    missing = [row.old_raw for row in rows if row.old_raw != row.new_raw and row.old_raw not in found]
    if missing:
        raise RegistryError(f"items.csv rows not found for migrated IDs: {missing[:5]}")
    for pool, begin, end in (
        (MIGRATION_POOLS[0], _POOL_A_BEGIN, _POOL_A_END),
        (MIGRATION_POOLS[1], _POOL_B_BEGIN, _POOL_B_END),
    ):
        block = _render_pool(rows, pool["subgroups"])
        lines = _replace_region(lines, begin, end, block)
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def _write_map(path: str, rows: Sequence[Any]) -> None:
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["old_id", "new_id", "name", "old_packed", "new_packed", "subgroup"])
        for row in rows:
            writer.writerow([row.old_raw, row.new_raw, row.name,
                             row.old_packed, row.new_packed, row.subgroup])


def main(argv: Any = None) -> int:
    parser = argparse.ArgumentParser(prog="item_registry_cli")
    sub = parser.add_subparsers(dest="command", required=True)
    validate = sub.add_parser("validate", help="strictly validate items.csv and an optional manifest")
    validate.add_argument("--items", required=True)
    validate.add_argument("--manifest")
    validate.add_argument("--json", action="store_true")
    validate.set_defaults(handler=_validate)
    stats = sub.add_parser("stats", help="show allocation capacity and free ranges")
    stats.add_argument("--items", required=True)
    stats.add_argument("--manifest", required=True)
    stats.add_argument("--json", action="store_true")
    stats.set_defaults(handler=_stats)
    migrate = sub.add_parser("migrate", help="renumber the 0:1110 and 1110 item ID pools")
    migrate.add_argument("--items", required=True)
    mode = migrate.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", action="store_true", help="print the mapping table (default)")
    mode.add_argument("--apply", action="store_true", help="rewrite items.csv and write the map")
    migrate.add_argument("--map", default=DEFAULT_MAP_PATH,
                        help=f"map CSV output path (default {DEFAULT_MAP_PATH})")
    migrate.set_defaults(handler=_migrate)
    args = parser.parse_args(argv)
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())
