#!/usr/bin/env python3
"""Pure item-registry parsing, validation, and allocation accounting helpers."""
from __future__ import annotations

import csv
import json
import re
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, NamedTuple, Optional, Sequence, Tuple, Union

Item = Tuple[str, str, str, str]
MAX_BITS = 15
ADDRESS_SPACE = 1 << 16


class RegistryError(ValueError):
    """Raised when a registry value does not satisfy the strict grammar."""


@dataclass(frozen=True)
class AllocationPrefix:
    raw: str
    bits: str

    @property
    def length(self) -> int:
        return len(self.bits)


@dataclass(frozen=True)
class PackedRange:
    start: int
    end: int

    def __post_init__(self) -> None:
        if not 0 <= self.start < self.end <= ADDRESS_SPACE:
            raise RegistryError(f"invalid packed range [{self.start}, {self.end})")

    @property
    def capacity(self) -> int:
        return self.end - self.start

    def contains(self, value: Any) -> bool:
        if isinstance(value, PackedRange):
            return self.start <= value.start and value.end <= self.end
        if isinstance(value, ParsedItemId):
            value = value.packed
        return isinstance(value, int) and self.start <= value < self.end


@dataclass(frozen=True)
class ParsedItemId:
    raw: str
    prefix: AllocationPrefix
    payload: int
    packed: int


def _strict_text(text: Any, what: str) -> str:
    if not isinstance(text, str) or not text or text != text.strip():
        raise RegistryError(f"{what} must be a non-empty string without surrounding whitespace")
    return text


def parse_allocation_prefix(text: str) -> AllocationPrefix:
    """Parse only binary prefix segments; no segment is a payload."""
    raw = _strict_text(text, "allocation prefix")
    segments = raw.split(":")
    if any(not segment for segment in segments):
        raise RegistryError(f"allocation prefix has an empty segment: {raw!r}")
    if any(set(segment) - {"0", "1"} for segment in segments):
        raise RegistryError(f"allocation prefix is not binary: {raw!r}")
    bits = "".join(segments)
    if not 1 <= len(bits) <= MAX_BITS:
        raise RegistryError(f"allocation prefix must contain 1..{MAX_BITS} bits: {raw!r}")
    return AllocationPrefix(raw, bits)


def parse_item_id(text: str) -> ParsedItemId:
    """Parse a complete ID, whose final colon-separated segment is decimal payload."""
    raw = _strict_text(text, "item ID")
    segments = raw.split(":")
    if len(segments) < 2:
        raise RegistryError(f"item ID needs a binary prefix and decimal payload: {raw!r}")
    payload_text = segments[-1]
    if not re.fullmatch(r"[0-9]+", payload_text):
        raise RegistryError(f"item ID payload is not decimal: {raw!r}")
    prefix = parse_allocation_prefix(":".join(segments[:-1]))
    payload = int(payload_text, 10)
    capacity = allocation_capacity(prefix)
    if payload >= capacity:
        raise RegistryError(f"payload {payload} exceeds capacity {capacity} for prefix {prefix.bits}")
    packed = (int(prefix.bits, 2) << (16 - prefix.length)) | payload
    return ParsedItemId(raw, prefix, payload, packed)


def allocation_capacity(prefix: Union[AllocationPrefix, str]) -> int:
    p = parse_allocation_prefix(prefix) if isinstance(prefix, str) else prefix
    return 1 << (16 - len(p.bits))


def allocation_range(prefix: Union[AllocationPrefix, str]) -> PackedRange:
    p = parse_allocation_prefix(prefix) if isinstance(prefix, str) else prefix
    start = int(p.bits, 2) << (16 - len(p.bits))
    return PackedRange(start, start + allocation_capacity(p))


def prefix_contains(parent: Union[AllocationPrefix, str], child: Union[AllocationPrefix, str]) -> bool:
    p = parse_allocation_prefix(parent) if isinstance(parent, str) else parent
    c = parse_allocation_prefix(child) if isinstance(child, str) else child
    return c.bits.startswith(p.bits)


def _interval(value: Any) -> Tuple[int, int]:
    if isinstance(value, PackedRange):
        return value.start, value.end
    if isinstance(value, ParsedItemId):
        return value.packed, value.packed + 1
    if isinstance(value, int):
        return value, value + 1
    if isinstance(value, (tuple, list)) and len(value) == 2:
        return int(value[0]), int(value[1])
    raise RegistryError(f"invalid occupied interval: {value!r}")


def free_intervals(container: Union[PackedRange, AllocationPrefix, str], occupied: Iterable[Any]) -> List[PackedRange]:
    """Subtract singleton IDs and half-open ranges from a container."""
    box = allocation_range(container) if isinstance(container, (str, AllocationPrefix)) else container
    intervals = sorted(_interval(v) for v in occupied)
    merged: List[Tuple[int, int]] = []
    for start, end in intervals:
        if start < box.start or end > box.end or start >= end:
            raise RegistryError(f"occupied interval [{start}, {end}) is outside [{box.start}, {box.end})")
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    result: List[PackedRange] = []
    cursor = box.start
    for start, end in merged:
        if cursor < start:
            result.append(PackedRange(cursor, start))
        cursor = max(cursor, end)
    if cursor < box.end:
        result.append(PackedRange(cursor, box.end))
    return result


def subtract_reserved_ranges(container: Union[PackedRange, AllocationPrefix, str], reserved: Iterable[Any]) -> List[PackedRange]:
    return free_intervals(container, reserved)


def _csv_rows(path: Union[str, Path]) -> Tuple[List[Item], List[str]]:
    items: List[Item] = []
    errors: List[str] = []
    try:
        handle = open(path, newline="", encoding="utf-8")
    except OSError as exc:
        return [], [f"cannot open {path}: {exc}"]
    with handle:
        reader = csv.reader(handle, strict=True)
        header_seen = False
        try:
            for line, row in enumerate(reader, 1):
                if not row or not any(cell.strip() for cell in row):
                    continue
                if row[0].lstrip().startswith("#"):
                    continue
                if not header_seen:
                    header_seen = True
                    if [cell.strip().lower() for cell in row] != ["int", "name", "stack", "meta"]:
                        errors.append(f"line {line}: expected header int,name,stack,meta")
                        # Keep validating the row as data for useful diagnostics.
                    else:
                        continue
                if len(row) != 4:
                    errors.append(f"line {line}: expected 4 CSV fields, got {len(row)}")
                    continue
                item = tuple(cell.strip() for cell in row)  # type: ignore[assignment]
                if not item[0] or not item[1]:
                    errors.append(f"line {line}: item ID and name are required")
                    continue
                items.append(item)
        except csv.Error as exc:
            errors.append(f"CSV syntax error: {exc}")
    return items, errors


def parse_csv(path: str) -> Tuple[List[Item], Dict[str, List[Item]], Dict[str, str]]:
    """Compatibility parser used by the GUI; it now honors CSV quoting and headers."""
    items, _ = _csv_rows(path)
    grouped: Dict[str, List[Item]] = defaultdict(list)
    labels: Dict[str, str] = {"": "root"}
    with open(path, newline="", encoding="utf-8") as handle:
        for row in csv.reader(handle):
            if not row or not any(cell.strip() for cell in row):
                continue
            line = row[0].strip()
            if line.startswith("#"):
                match = re.match(r"#\s*(.+?)\s*\(prefix\s+([^)]*)\)", line)
                if match:
                    labels[match.group(2)] = match.group(1).strip()
    for item in items:
        prefix = ":".join(item[0].split(":")[:-1])
        grouped[prefix].append(item)
    return items, dict(grouped), labels


def build_tree(grouped: Dict[str, List[Item]]) -> dict:
    tree: dict = {}
    for prefix in sorted(grouped.keys()):
        node = tree
        if prefix:
            for seg in prefix.split(":"):
                node = node.setdefault(seg, {})
        node["__items__"] = grouped[prefix]
    return tree


def subtree_count(node: dict) -> int:
    return sum(len(value) if key == "__items__" else subtree_count(value)
               for key, value in node.items())


def validate_ids(items: List[Item]) -> List[str]:
    return validate_items_strict(items)


def validate_items_strict(source: Union[str, Path, Sequence[Item]]) -> List[str]:
    """Return deterministic strict validation diagnostics for CSV items/rows."""
    if isinstance(source, (str, Path)):
        items, errors = _csv_rows(source)
    else:
        items, errors = list(source), []
    seen: Dict[int, str] = {}
    for index, item in enumerate(items, 1):
        if len(item) < 2:
            errors.append(f"row {index}: expected item ID and name")
            continue
        raw = item[0]
        try:
            parsed = parse_item_id(raw)
        except RegistryError as exc:
            errors.append(f"row {index} {raw!r}: {exc}")
            continue
        if parsed.packed in seen:
            errors.append(f"row {index} {raw!r}: duplicate packed ID {parsed.packed} (already {seen[parsed.packed]!r})")
        else:
            seen[parsed.packed] = raw
    return sorted(errors)


def find_next_id(grouped: Dict[str, List[Item]], prefix: str) -> str:
    items = grouped.get(prefix, [])
    existing = set()
    for item_id, *_ in items:
        try:
            existing.add(parse_item_id(item_id).payload)
        except RegistryError:
            continue
    for i in range(1 << 16):
        if i not in existing:
            return f"{prefix}:{i}" if prefix else str(i)
    raise RegistryError(f"allocation range {prefix!r} is full")


def _normalize_path(path: Any) -> str:
    if not isinstance(path, str) or not path or path != path.strip():
        raise RegistryError("group path must be a non-empty string")
    value = path.replace("\\", "/").strip("/")
    parts = value.split("/")
    if any(not part or part in {".", ".."} for part in parts):
        raise RegistryError(f"invalid group path: {path!r}")
    return "/".join(parts)


def parse_manifest(source: Union[str, Path, Mapping[str, Any]]) -> dict:
    try:
        data = json.loads(Path(source).read_text(encoding="utf-8")) if isinstance(source, (str, Path)) else dict(source)
    except (OSError, json.JSONDecodeError) as exc:
        raise RegistryError(f"cannot parse manifest: {exc}") from exc
    if data.get("version") != 1 or not isinstance(data.get("groups"), list):
        raise RegistryError("manifest requires version 1 and a groups list")
    groups = []
    paths = set()
    prefixes: Dict[str, str] = {}
    for raw in data["groups"]:
        if not isinstance(raw, dict):
            raise RegistryError("manifest group must be an object")
        path = _normalize_path(raw.get("path"))
        if path in paths:
            raise RegistryError(f"duplicate group path: {path}")
        paths.add(path)
        prefix_raw = raw.get("allocation_prefix")
        if prefix_raw is None:
            prefix = None
        elif isinstance(prefix_raw, AllocationPrefix):
            prefix = prefix_raw
        else:
            prefix = parse_allocation_prefix(prefix_raw)
        if prefix is not None and prefix.bits in prefixes:
            raise RegistryError(f"duplicate canonical allocation prefix {prefix.bits}: {prefixes[prefix.bits]} and {path}")
        if prefix is not None:
            prefixes[prefix.bits] = path
        ids = raw.get("item_ids")
        if not isinstance(ids, list) or any(not isinstance(item, str) for item in ids):
            raise RegistryError(f"group {path} requires item_ids list")
        ratio = raw.get("reserve_ratio", 0.0)
        if isinstance(ratio, bool) or not isinstance(ratio, (int, float)) or not 0 <= ratio <= 1:
            raise RegistryError(f"group {path} reserve_ratio must be between 0 and 1")
        groups.append({"path": path, "allocation_prefix": prefix, "item_ids": list(ids), "reserve_ratio": float(ratio)})

    anchors = []
    for raw_anchor in data.get("legacy_anchors", []):
        if not isinstance(raw_anchor, dict):
            raise RegistryError("legacy anchor must be an object")
        try:
            raw_bits = raw_anchor.get("bits")
            prefix = raw_bits if isinstance(raw_bits, AllocationPrefix) else parse_allocation_prefix(raw_bits)
            expected = allocation_range(prefix)
            raw_range = raw_anchor.get("range")
            actual = raw_range if isinstance(raw_range, PackedRange) else None
            if actual is None and (not isinstance(raw_range, list) or len(raw_range) != 2):
                raise RegistryError("legacy anchor range must be [start, end]")
            if actual is None:
                actual = PackedRange(int(raw_range[0]), int(raw_range[1]))
            if actual != expected:
                raise RegistryError(f"legacy anchor {prefix.bits} range does not match prefix")
        except (TypeError, ValueError, RegistryError) as exc:
            raise RegistryError(f"invalid legacy anchor: {exc}") from exc
        anchors.append({"bits": prefix, "range": actual, "source": str(raw_anchor.get("source", ""))})

    anchors.sort(key=lambda anchor: (anchor["range"].start, anchor["range"].end))
    for left, right in zip(anchors, anchors[1:]):
        if left["range"].end > right["range"].start:
            raise RegistryError(f"overlapping legacy anchors: {left['bits'].bits} and {right['bits'].bits}")
    groups.sort(key=lambda group: group["path"])
    return {"version": 1, "groups": groups, "legacy_anchors": anchors}


def _nearest_allocated_ancestor(groups: Sequence[Mapping[str, Any]], path: str) -> Optional[Mapping[str, Any]]:
    ancestors = [
        group for group in groups
        if group["path"] != path
        and path.startswith(group["path"] + "/")
        and group["allocation_prefix"] is not None
    ]
    return max(ancestors, key=lambda group: len(group["path"]), default=None)


def _allocation_children(groups: Sequence[Mapping[str, Any]], parent: Mapping[str, Any]) -> List[Mapping[str, Any]]:
    children = []
    parent_path = parent["path"]
    for group in groups:
        if group is parent or not group["path"].startswith(parent_path + "/"):
            continue
        if group["allocation_prefix"] is None:
            continue
        nearest = _nearest_allocated_ancestor(groups, group["path"])
        if nearest is parent:
            children.append(group)
    return children


def validate_manifest(manifest: Union[str, Path, Mapping[str, Any]], items: Sequence[Item] = ()) -> List[str]:
    try:
        data = parse_manifest(manifest)
    except RegistryError as exc:
        return [str(exc)]
    errors: List[str] = []
    parsed_items: Dict[int, str] = {}
    for row in items:
        try:
            item = parse_item_id(row[0])
            if item.packed in parsed_items:
                errors.append(f"items.csv has duplicate packed ID {item.packed}")
            else:
                parsed_items[item.packed] = row[0]
        except (RegistryError, IndexError):
            continue

    groups = data["groups"]
    paths = {group["path"] for group in groups}
    for path in paths:
        parent = path.rpartition("/")[0]
        if parent and parent not in paths:
            errors.append(f"group {path}: missing logical parent {parent}")

    owned: Dict[int, str] = {}
    allocated = [(group, group["allocation_prefix"]) for group in groups if group["allocation_prefix"]]
    for group in groups:
        path, prefix = group["path"], group["allocation_prefix"]
        nearest = _nearest_allocated_ancestor(groups, path)
        if prefix is not None and nearest is not None and not prefix_contains(nearest["allocation_prefix"], prefix):
            errors.append(f"group {path}: allocation prefix {prefix.bits} is outside parent {nearest['path']}")
        effective_range = allocation_range(prefix or nearest["allocation_prefix"]) if (prefix or nearest) else None
        child_groups = _allocation_children(groups, group) if prefix is not None else []
        child_ranges = [allocation_range(child["allocation_prefix"]) for child in child_groups]
        for raw_id in group["item_ids"]:
            try:
                parsed = parse_item_id(raw_id)
            except RegistryError as exc:
                errors.append(f"group {path} item {raw_id!r}: {exc}")
                continue
            if parsed.packed not in parsed_items:
                errors.append(f"group {path} item {raw_id!r} does not resolve to items.csv")
            elif parsed_items[parsed.packed] != raw_id:
                errors.append(f"group {path} item {raw_id!r} is an alias for {parsed_items[parsed.packed]!r}")
            if parsed.packed in owned:
                errors.append(f"item {raw_id!r} assigned to groups {owned[parsed.packed]} and {path}")
            owned[parsed.packed] = path
            if effective_range is not None and not effective_range.contains(parsed.packed):
                errors.append(f"group {path} item {raw_id!r} is outside allocation prefix {effective_range}")
            if any(child_range.contains(parsed.packed) for child_range in child_ranges):
                errors.append(f"group {path} item {raw_id!r} is inside a child allocation range")

    for packed, raw_id in parsed_items.items():
        if packed not in owned:
            errors.append(f"items.csv item {raw_id!r} is not assigned to any manifest group")

    for i, (left, lp) in enumerate(allocated):
        for right, rp in allocated[i + 1:]:
            if lp.bits == rp.bits:
                continue  # parse_manifest already reports aliases
            lr, rr = allocation_range(lp), allocation_range(rp)
            if not lr.contains(rr) and not rr.contains(lr):
                continue
            if prefix_contains(lp, rp):
                ancestor, descendant = left, right
            else:
                ancestor, descendant = right, left
            nearest = _nearest_allocated_ancestor(groups, descendant["path"])
            if nearest is not ancestor:
                errors.append(f"overlapping allocation ranges without logical parent: {left['path']} and {right['path']}")

    return sorted(set(errors))


@dataclass(frozen=True)
class MigrationRow:
    old_raw: str
    new_raw: str
    name: str
    old_packed: int
    new_packed: int
    subgroup: str
    stack: str
    meta: str


# Deterministic renumbering spec for the two messy pools (processed materials
# under 0:1110, machines under 1110) into binary-aligned child prefixes.
# Membership is by explicit item name; within a subgroup items keep their
# relative order sorted by old packed ID and the new payload is the index.
MIGRATION_POOLS: Tuple[Mapping[str, Any], ...] = (
    {
        "parent": "0:1110",
        "subgroups": (
            ("base/processed/plates", "0:1110:000", (
                "bronze_plate", "aluminium_plate", "brass_plate", "chromium_plate",
                "copper_plate", "gold_plate", "invar_plate", "iridium_plate",
                "iron_plate", "lead_plate", "osmium_plate", "platinum_plate",
                "silver_plate", "stainless_steel_plate", "steel_plate", "tin_plate",
                "titanium_plate", "tungsten_plate", "tungstensteel_plate",
                "zinc_plate", "wooden_pressure_plate", "graphene", "carbon_fiber",
            )),
            ("base/processed/dusts", "0:1110:001", (
                "tin_dust", "copper_dust", "bronze_dust", "iron_dust", "gold_dust",
                "electrum_dust", "uranium_dust", "aluminium_dust", "brass_dust",
                "carbon_dust", "chromium_dust", "coal_dust", "invar_dust",
                "iridium_dust", "lead_dust", "osmium_dust", "platinum_dust",
                "silicon_dust", "silver_dust", "steel_dust", "titanium_dust",
                "tungsten_dust", "zinc_dust", "crushed_copper", "crushed_gold",
                "crushed_iron", "crushed_lead", "crushed_silver", "crushed_tin",
                "crushed_zinc",
            )),
            ("base/processed/rods-bolts-screws", "0:1110:010", (
                "rod", "copper_rod", "iron_rod", "steel_rod", "tungsten_rod",
            )),
            ("base/processed/gears-rings-foils-springs-rotors", "0:1110:011", (
                "copper_ring", "iron_gear", "steel_gear", "steel_ring",
                "steel_rotor", "steel_spring", "titanium_rotor",
            )),
            ("base/processed/wires", "0:1110:100", ()),
            ("base/processed/gems-crystals", "0:1110:101", (
                "quartz", "crystal", "redstone", "lapis", "diamond",
            )),
        ),
    },
    {
        "parent": "1110",
        "subgroups": (
            ("machines/heat-processing", "1110:000", (
                "heat_furnace", "heat_macerator", "heat_generator",
            )),
            ("machines/steam-processing", "1110:001", (
                "steam_macerator", "steam_compressor", "steam_extractor",
                "steam_mixer", "bronze_alloy_smelter", "rotare_macerator",
            )),
            ("machines/electric-processing", "1110:010", (
                "alloy_smelter_lv", "bender", "centrifuge_lv",
                "chemical_reactor_lv", "compressor_lv", "electric_furnace_hv",
                "electric_furnace_lv", "electric_furnace_mv", "electrolyzer_lv",
                "extractor_lv", "extruder", "macerator_lv", "mixer_lv", "wiremill",
                "forge_hammer_lv", "ore_washer_lv", "thermal_centrifuge_lv",
                "assembling_machine_lv", "macerator_mv", "compressor_mv",
                "extractor_mv", "mixer_mv", "alloy_smelter_mv", "assembling_machine_mv",
                "assembling_machine_hv", "circuit_assembler", "precision_laser_welder",
                "implosion_compressor", "vacuum_freezer", "industrial_smelter",
                "industrial_grinder", "industrial_squeezer", "industrial_brewery",
                "large_turbine", "multi_smelter", "cracker", "distillation_tower",
                "chemical_plant", "pyrolyse_oven", "large_combustion_reactor",
                "assembly_line", "extreme_combustion_reactor", "naquadah_reactor",
                "coke_oven", "steam_turbine", "large_chemical_reactor", "advanced_database",
            )),
            ("machines/boilers-generation", "1110:011", (
                "steam_solid_boiler", "steam_heat_boiler",
            )),
            ("machines/generation", "1110:100", (
                "creative_generator", "rotare_generator",
            )),
            ("machines/storage", "1110:101", (
                "battery_buffer_lv", "battery_buffer_mv", "battery_buffer_hv",
                "charger", "fluid_tank", "battery_buffer_ev", "battery_buffer_iv",
            )),
            ("machines/transformers", "1110:110", (
                "transformer_mv_hv", "transformer_hv_ev", "transformer_lv_mv",
                "transformer_ev_iv", "transformer_iv_luv",
            )),
            ("machines/multiblock-parts-casings", "1110:111", (
                "hull_lv", "hull_mv", "hull_hv", "hull_luv", "hull_zpm",
                "hatch_input_lv", "hatch_output_lv", "hatch_bus_input_lv",
                "hatch_bus_output_lv", "hatch_energy_input_lv", "hatch_maintenance",
                "multi_volume_hatch",
            )),
        ),
    },
)


def _migration_membership() -> Dict[str, Tuple[str, str]]:
    membership: Dict[str, Tuple[str, str]] = {}
    for pool in MIGRATION_POOLS:
        for subgroup, prefix, names in pool["subgroups"]:
            for name in names:
                if name in membership:
                    raise RegistryError(
                        f"migration spec assigns {name!r} to both "
                        f"{membership[name][0]!r} and {subgroup!r}")
                membership[name] = (subgroup, prefix)
    return membership


def build_migration(items: Sequence[Item]) -> List[MigrationRow]:
    """Compute the deterministic old→new ID mapping for the two migrated pools.

    Every item whose ID prefix starts with a migrated pool parent must be
    classified into exactly one subgroup; all other items are left untouched.
    Raises RegistryError on any uniqueness/containment violation.
    """
    membership = _migration_membership()
    rows: List[MigrationRow] = []
    seen_names: Dict[str, str] = {}
    for item in items:
        raw, name, stack, meta = (tuple(item) + ("", "", "", ""))[:4]
        if name in seen_names:
            raise RegistryError(f"duplicate item name {name!r} ({seen_names[name]!r} and {raw!r})")
        seen_names[name] = raw
        try:
            parsed = parse_item_id(raw)
        except RegistryError:
            continue
        for pool in MIGRATION_POOLS:
            parent = parse_allocation_prefix(pool["parent"])
            if not parsed.prefix.bits.startswith(parent.bits):
                continue
            if name not in membership:
                raise RegistryError(
                    f"item {name!r} ({raw!r}) is under migrated pool {pool['parent']!r} "
                    "but is not classified in any subgroup")
            subgroup, prefix_raw = membership[name]
            if parsed.prefix.bits == parse_allocation_prefix(prefix_raw).bits:
                # Already-migrated item: keep its current ID (idempotent re-run).
                rows.append(MigrationRow(raw, raw, name, parsed.packed, parsed.packed, subgroup, stack, meta))
            else:
                rows.append(MigrationRow(raw, "", name, parsed.packed, -1, subgroup, stack, meta))
            break

    # Assign new payloads per subgroup, ordered by old packed ID.
    by_subgroup: Dict[str, List[MigrationRow]] = defaultdict(list)
    for row in rows:
        if row.new_packed == -1:
            by_subgroup[row.subgroup].append(row)
    new_rows: List[MigrationRow] = []
    for pool in MIGRATION_POOLS:
        parent_range = allocation_range(pool["parent"])
        child_ranges = [allocation_range(prefix) for _, prefix, _ in pool["subgroups"]]
        for left, right in zip(child_ranges, child_ranges[1:]):
            if left.end > right.start:
                raise RegistryError(f"overlapping child ranges {left} and {right}")
        for child in child_ranges:
            if not parent_range.contains(child):
                raise RegistryError(f"child range {child} is outside parent {pool['parent']!r}")
        for subgroup, prefix_raw, _ in pool["subgroups"]:
            group_rows = sorted(by_subgroup.get(subgroup, []), key=lambda row: row.old_packed)
            for index, row in enumerate(group_rows):
                new_raw = f"{prefix_raw}:{index}"
                new_rows.append(MigrationRow(
                    row.old_raw, new_raw, row.name, row.old_packed,
                    parse_item_id(new_raw).packed, row.subgroup, row.stack, row.meta))
    for row in rows:
        if row.new_packed != -1:
            new_rows.append(row)

    new_packed_seen: Dict[int, str] = {}
    for row in new_rows:
        if row.new_packed in new_packed_seen:
            raise RegistryError(
                f"duplicate new packed ID {row.new_packed} for {row.name!r} "
                f"(already {new_packed_seen[row.new_packed]!r})")
        new_packed_seen[row.new_packed] = row.name
    return sorted(new_rows, key=lambda row: (row.subgroup, row.new_packed))


def registry_stats(items: Sequence[Item], manifest: Mapping[str, Any]) -> List[dict]:
    normalized = parse_manifest(manifest)
    groups = normalized["groups"]
    for group in groups:
        if isinstance(group["allocation_prefix"], str):
            group["allocation_prefix"] = parse_allocation_prefix(group["allocation_prefix"])  # type: ignore[assignment]
    parsed = []
    for row in items:
        try:
            parsed.append(parse_item_id(row[0]))
        except RegistryError:
            pass
    by_path = {g["path"]: g for g in groups}
    out = []
    for group in groups:
        prefix = group["allocation_prefix"]
        if prefix is None:
            continue
        rng = allocation_range(prefix)
        children = _allocation_children(groups, group)
        child_ranges = [allocation_range(child["allocation_prefix"]) for child in children]
        direct_ids = [p.packed for p in parsed if any(i == p.raw for i in group["item_ids"])]
        descendant_groups = [
            child for child in groups
            if child is not group and child["path"].startswith(group["path"] + "/")
        ]
        descendant_ids = [
            p.packed for p in parsed
            if any(i == p.raw for child in descendant_groups for i in child["item_ids"])
        ]
        legacy_ranges = [
            anchor["range"] for anchor in normalized.get("legacy_anchors", [])
            if rng.contains(anchor["range"])
        ]
        free = free_intervals(rng, child_ranges + legacy_ranges + direct_ids)
        reserved = rng.capacity - sum(interval.capacity for interval in free_intervals(rng, child_ranges + legacy_ranges))
        capacity = rng.capacity
        configured = int(capacity * group["reserve_ratio"])
        available = sum(interval.capacity for interval in free)
        out.append({
            "path": group["path"],
            "prefix": prefix.bits,
            "range": [rng.start, rng.end],
            "capacity": capacity,
            "direct_usage": len(direct_ids),
            "descendant_usage": len(descendant_ids),
            "child_reservations": reserved,
            "free_ranges": [[interval.start, interval.end] for interval in free],
            "headroom": {"configured": configured, "absolute": available},
        })
    return sorted(out, key=lambda row: row["path"])
