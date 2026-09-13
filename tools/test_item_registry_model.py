import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tools.editor_model import (
    MIGRATION_POOLS,
    RegistryError,
    allocation_capacity,
    allocation_range,
    build_migration,
    free_intervals,
    parse_allocation_prefix,
    parse_csv,
    parse_item_id,
    prefix_contains,
    registry_stats,
    validate_manifest,
    validate_items_strict,
)

REPO_ROOT = Path(__file__).resolve().parent.parent
ITEMS_CSV = REPO_ROOT / "data" / "registry" / "items.csv"


class ItemRegistryMigrationTests(unittest.TestCase):
    def _rows_for(self, entries):
        items = [(old, name, stack, meta) for old, name, stack, meta in entries]
        return build_migration(items)

    def _synthetic_entries(self):
        # One item per subgroup covering both pools, plus an untouched item.
        entries = [("0:0:1", "stone", "", "0")]
        for pool in MIGRATION_POOLS:
            for index, (subgroup, prefix, names) in enumerate(pool["subgroups"]):
                for offset, name in enumerate(names):
                    payload = 100 + offset if names else 0
                    entries.append((f"{pool['parent']}:{payload + index}", name, "", "0"))
        return entries

    def test_synthetic_migration_unique_and_contained(self):
        rows = self._rows_for(self._synthetic_entries())
        spec_names = {name for pool in MIGRATION_POOLS for _, _, names in pool["subgroups"] for name in names}
        self.assertEqual({row.name for row in rows}, spec_names)
        new_packed = [row.new_packed for row in rows]
        self.assertEqual(len(new_packed), len(set(new_packed)))
        for row in rows:
            prefix = row.new_raw.rsplit(":", 1)[0]
            self.assertTrue(allocation_range(prefix).contains(row.new_packed))

    def test_unclassified_pool_item_is_rejected(self):
        entries = [("0:1110:7", "mystery_ore", "", "0")]
        with self.assertRaises(RegistryError):
            build_migration(entries)

    def test_determinism_and_identity_on_real_registry(self):
        items, _, _ = parse_csv(ITEMS_CSV)
        first = build_migration(list(items))
        second = build_migration(list(items))
        self.assertEqual(first, second)
        # Round-trip: feeding the migrated registry back must be identity.
        migrated = {(row.new_raw, row.name, row.stack, row.meta) for row in first}
        rerun = build_migration(sorted(migrated))
        by_name = {row.name: row for row in rerun}
        for row in first:
            self.assertEqual(by_name[row.name].new_raw, row.new_raw)
        # Non-pool items are never present in migration rows.
        pool_names = {name for pool in MIGRATION_POOLS for _, _, names in pool["subgroups"] for name in names}
        self.assertTrue(all(row.name in pool_names for row in first))

    def test_real_registry_subgroups_exactly_cover_spec(self):
        items, _, _ = parse_csv(ITEMS_CSV)
        rows = build_migration(list(items))
        expected = {}
        for pool in MIGRATION_POOLS:
            for subgroup, prefix, names in pool["subgroups"]:
                expected[subgroup] = (prefix, len(names))
        counts = {}
        prefixes = {}
        for row in rows:
            counts[row.subgroup] = counts.get(row.subgroup, 0) + 1
            prefixes[row.subgroup] = row.new_raw.rsplit(":", 1)[0]
        for subgroup, (prefix, count) in expected.items():
            if count == 0:
                continue
            self.assertEqual(counts.get(subgroup), count, subgroup)
            self.assertEqual(prefixes.get(subgroup), prefix, subgroup)


class ItemRegistryModelTests(unittest.TestCase):
    def test_equivalent_prefixes_and_appended_payload(self):
        self.assertEqual(parse_allocation_prefix("0:1:0").bits, "010")
        self.assertEqual(parse_allocation_prefix("0:10").bits, "010")
        self.assertEqual(parse_item_id("0:1:0:7").packed, parse_item_id("0:10:7").packed)
        self.assertTrue(allocation_range("0:10").contains(parse_item_id("0:1:0:7")))
        with self.assertRaises(RegistryError):
            parse_allocation_prefix("0:10:7")

    def test_capacity_boundaries(self):
        for length in (1, 2, 6, 10, 15):
            prefix = "1" * length
            capacity = allocation_capacity(prefix)
            self.assertEqual(parse_item_id(f"{prefix}:0").payload, 0)
            self.assertEqual(parse_item_id(f"{prefix}:{capacity - 1}").payload, capacity - 1)
            with self.assertRaises(RegistryError):
                parse_item_id(f"{prefix}:{capacity}")

    def test_prefix_containment_and_free_ranges(self):
        self.assertTrue(prefix_contains("0:110", "0:110:10"))
        self.assertFalse(prefix_contains("0:110", "0:111"))
        parent = allocation_range("0:110")
        child = allocation_range("0:110:10")
        free = free_intervals(parent, [child, parent.start])
        self.assertEqual(free[0].start, parent.start + 1)
        self.assertEqual(free[-1].end, parent.end)

    def test_parent_direct_items_and_child_reservation(self):
        items = [("0:0:1", "parent", "", "0"), ("0:0:10:1", "child", "", "0")]
        manifest = {
            "version": 1,
            "groups": [
                {"path": "root", "allocation_prefix": "0:0", "item_ids": ["0:0:1"], "reserve_ratio": 0},
                {"path": "root/child", "allocation_prefix": "0:0:10", "item_ids": ["0:0:10:1"], "reserve_ratio": 0},
            ],
        }
        self.assertEqual(validate_manifest(manifest, items), [])
        stats = registry_stats(items, manifest)
        self.assertEqual(stats[0]["child_reservations"], allocation_capacity("0:0:10"))
        self.assertEqual(stats[0]["direct_usage"], 1)

    def test_arbitrary_logical_depth_shared_pool(self):
        items = [("0:0:1", "item", "", "0")]
        manifest = {"version": 1, "groups": [
            {"path": "a", "allocation_prefix": "0:0", "item_ids": [], "reserve_ratio": 0},
            {"path": "a/b", "allocation_prefix": None, "item_ids": [], "reserve_ratio": 0},
            {"path": "a/b/c", "allocation_prefix": None, "item_ids": [], "reserve_ratio": 0},
            {"path": "a/b/c/d", "allocation_prefix": None, "item_ids": ["0:0:1"], "reserve_ratio": 0},
        ]}
        self.assertEqual(validate_manifest(manifest, items), [])

    def test_manifest_legacy_anchor_ranges(self):
        manifest = {"version": 1, "legacy_anchors": [{"bits": "0:10", "range": [16384, 24576]}], "groups": [
            {"path": "root", "allocation_prefix": "0", "item_ids": [], "reserve_ratio": 0},
        ]}
        self.assertEqual(validate_manifest(manifest, []), [])
        self.assertEqual(registry_stats([], manifest)[0]["child_reservations"], 8192)

    def test_manifest_duplicate_and_containment(self):
        duplicate = {"version": 1, "groups": [
            {"path": "a", "allocation_prefix": "0:1:0", "item_ids": [], "reserve_ratio": 0},
            {"path": "b", "allocation_prefix": "0:10", "item_ids": [], "reserve_ratio": 0},
        ]}
        self.assertTrue(any("duplicate canonical" in error for error in validate_manifest(duplicate)))
        outside = {"version": 1, "groups": [
            {"path": "a", "allocation_prefix": "0:0", "item_ids": [], "reserve_ratio": 0},
            {"path": "a/b", "allocation_prefix": "1", "item_ids": [], "reserve_ratio": 0},
        ]}
        self.assertTrue(any("outside parent" in error for error in validate_manifest(outside)))

    def test_strict_csv_and_collisions(self):
        rows = [("0:1:0:7", 'name,with comma', "", "0"), ("0:10:7", "alias", "", "0")]
        errors = validate_items_strict(rows)
        self.assertTrue(any("duplicate packed ID" in error for error in errors))

    def test_cli_is_deterministic_and_read_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            items = root / "items.csv"
            manifest = root / "manifest.json"
            items.write_text("int,name,stack,meta\n0:0:1,stone,,0\n", encoding="utf-8")
            manifest.write_text(json.dumps({"version": 1, "groups": [{"path": "root", "allocation_prefix": "0:0", "item_ids": ["0:0:1"], "reserve_ratio": 0.25}]}), encoding="utf-8")
            before = (items.read_bytes(), manifest.read_bytes())
            command = [sys.executable, "-m", "tools.item_registry_cli", "stats", "--items", str(items), "--manifest", str(manifest), "--json"]
            first = subprocess.run(command, check=True, capture_output=True, text=True)
            second = subprocess.run(command, check=True, capture_output=True, text=True)
            self.assertEqual(first.stdout, second.stdout)
            self.assertEqual(before, (items.read_bytes(), manifest.read_bytes()))
            self.assertEqual(json.loads(first.stdout)[0]["capacity"], 16384)


if __name__ == "__main__":
    unittest.main()
