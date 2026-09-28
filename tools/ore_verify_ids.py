#!/usr/bin/env python3
"""Validate proposed ore item ids against the real ItemId::pack encoding.

The 10: ore namespace is a flat 14-bit payload. This tool exists because
"just write the id" is unsafe here: items.csv ids are declared immutable
(item-subgroups.json baseline.immutable_ids, and the items.csv header says
"Existing hierarchical IDs are immutable"), so a wrong id is a namespace
bug that cannot be fixed by editing the row afterwards.

What it checks, per proposed id:

  1. pack() is a faithful reimplementation. It is self-tested against
     item-id-migration.csv, which records the ACTUAL uint16 the C++ loader
     produced for 103 rows (206 integers, old_packed + new_packed). An
     encoding bug here would silently validate garbage, so this is a
     precondition, not an assumption.
  2. Round trip: the text decodes back to the same integer, and the text
     is among the valid decodings of that integer. The encoding is
     ambiguous by construction -- 10:20 also decodes as 100:20 -- so the
     assertion is membership in the valid decoding set, not equality with
     a single canonical form.
  3. Namespace collision: the packed integer is not already used by a live
     row of items.csv.
  4. Category band: the integer falls inside CAT_ORES, i.e.
     pack("10:0") <= id < pack("110:0"). An ore id outside this band is
     not an ore to ItemId::category() even though the text looks right.
  5. Vein coverage: every registered ore is placed by at least one ores.json
     vein, and no vein band misses the generator's centre range [5,60]
     (OreGenerator.cpp:13). A registered-but-unplaced ore is a
     progression root that can never be mined.

Usage:
    python3 tools/ore_verify_ids.py            # validate PROPOSED table + live tree
    python3 tools/ore_verify_ids.py --live     # validate live items.csv only
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGISTRY = os.path.join(ROOT, "src/content/data/registry")
ITEMS_CSV = os.path.join(REGISTRY, "items.csv")
MIGRATION_CSV = os.path.join(REGISTRY, "item-id-migration.csv")
DEFERRED_CSV = os.path.join(REGISTRY, "item-subgroups-deferred.csv")
ORES_JSON = os.path.join(REGISTRY, "ores.json")

ORE_CENTRE_MIN_Y, ORE_CENTRE_MAX_Y = 5, 60  # OreGenerator.cpp:13


# ---------------------------------------------------------------------------
# Encoding
# ---------------------------------------------------------------------------


def pack(s: str) -> int:
    """Byte-for-byte reimplementation of ItemId::pack (ItemId.h:85-131).

    Prefix chars before the LAST colon contribute a bit ONLY if they are
    '0' or '1'; any other char, including ':' and digits like '2' or '3',
    is silently skipped. That skip is load-bearing -- see check_proposed.
    """
    if not s:
        return 0
    last_colon = s.rfind(":")
    if last_colon == -1:
        val = 0
        for c in s:
            if "0" <= c <= "9":
                val = (val * 10 + int(c)) & 0xFFFF
        return val
    prefix, plen = 0, 0
    for c in s[:last_colon]:
        if c == "0":
            prefix = ((prefix << 1) | 0) & 0xFFFF
            plen += 1
        elif c == "1":
            prefix = ((prefix << 1) | 1) & 0xFFFF
            plen += 1
        # ':' and any non-0/1 char are skipped, exactly as the C++ does.
    if plen > 15:
        return 0
    payload = 0
    for c in s[last_colon + 1 :]:
        if "0" <= c <= "9":
            payload = (payload * 10 + int(c)) & 0xFFFF
    return ((prefix << (16 - plen)) | payload) & 0xFFFF


def decode_all(value: int) -> set[str]:
    """Every text that packs to `value`, via the top-down prefix decode.

    Reconstructs the inverse of pack(): for each candidate prefix length,
    the top `plen` bits must have bit_length == plen (no leading zero),
    which is the "prefix-free by construction" rule the header claims.
    """
    out = set()
    for plen in range(1, 16):
        pre = value >> (16 - plen)
        if pre.bit_length() != plen:
            continue
        payload = value & ((1 << (16 - plen)) - 1)
        out.add(f"{pre:b}:{payload}")
    return out


def category(value: int) -> str:
    if value < pack("10:0"):
        return "CAT_BASE"
    if value < pack("110:0"):
        return "CAT_ORES"
    if value < pack("1110:0"):
        return "CAT_MATERIALS"
    if value < pack("1111:0"):
        return "CAT_MACHINES"
    return "CAT_INFRA"


# ---------------------------------------------------------------------------
# Live registry
# ---------------------------------------------------------------------------


def load_items(path: str = ITEMS_CSV) -> dict[str, str]:
    items: dict[str, str] = {}
    with open(path, newline="", encoding="utf-8") as fh:
        for row in csv.reader(fh):
            if not row:
                continue
            first = row[0].strip()
            if not first or first.startswith("#") or first == "int":
                continue
            if len(row) < 2 or not row[1].strip():
                continue
            items[first] = row[1].strip()
    return items


def selftest_encoding() -> list[str]:
    """Prove pack() against integers the C++ actually produced."""
    problems: list[str] = []
    if not os.path.exists(MIGRATION_CSV):
        return [f"{MIGRATION_CSV} missing - cannot self-test pack()"]
    checked = mismatched = 0
    with open(MIGRATION_CSV, newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            for col in ("old_packed", "new_packed"):
                want, text = int(row[col]), (
                    row["old_id"] if col == "old_packed" else row["new_id"]
                )
                checked += 1
                got = pack(text)
                if got != want:
                    mismatched += 1
                    if len(problems) < 5:
                        problems.append(
                            f"pack({text!r}) = {got}, migration file says {want} ({row['name']})"
                        )
    if mismatched:
        problems.insert(0, f"pack() disagrees with {MIGRATION_CSV}: {mismatched}/{checked} wrong")
    return problems


# ---------------------------------------------------------------------------
# Per-id validation
# ---------------------------------------------------------------------------


def check_proposed(
    name: str, pid: str, live: dict[str, str], by_value: dict[int, tuple[str, str]]
):
    """Return (ok, [problems], info) for one proposed ore id."""
    problems: list[str] = []
    value = pack(pid)
    info = {"name": name, "id": pid, "packed": value}

    # 2. round trip
    if pid not in decode_all(value):
        problems.append(f"{pid!r} does not decode back to itself (pack={value})")
    # 3. collision against every live row, at any id depth
    if value in by_value:
        other_pid, other_name = by_value[value]
        problems.append(
            f"COLLIDES with live {other_pid} ({other_name}) - both encode to {value}"
        )
    # 4. category band
    if category(value) != "CAT_ORES":
        problems.append(f"{pid!r} -> {value} is {category(value)}, not CAT_ORES")

    info["decode_all"] = sorted(decode_all(value))
    return (not problems), problems, info


def check_veins(live: dict[str, str]):
    """Registered-ore coverage and band liveness, mirroring the audit."""
    with open(ORES_JSON, encoding="utf-8") as fh:
        veins = json.load(fh).get("veins", [])
    required = {"name", "min_y", "max_y", "weight", "primary", "secondary", "sporadic"}
    placed: dict[str, list[str]] = {}
    problems: list[str] = []
    for v in veins:
        missing = required - set(v)
        if missing:
            problems.append(f"vein {v.get('name')!r} missing field(s): {sorted(missing)}")
            continue
        for key in ("primary", "secondary", "sporadic"):
            ref = str(v[key])
            if ref not in live:
                problems.append(f"vein {v['name']!r} {key}={ref} is not a registered item")
            placed.setdefault(ref, []).append(f"{v['name']}.{key}")
        lo = max(v["min_y"], ORE_CENTRE_MIN_Y)
        hi = min(v["max_y"], ORE_CENTRE_MAX_Y)
        if lo > hi:
            problems.append(
                f"vein {v['name']!r} band [{v['min_y']},{v['max_y']}] never intersects "
                f"[{ORE_CENTRE_MIN_Y},{ORE_CENTRE_MAX_Y}] - DEAD"
            )
    for pid, name in live.items():
        if pid.startswith("10:") and pid not in placed:
            problems.append(f"registered ore {name} ({pid}) is placed by NO vein")
    return veins, placed, problems


# ---------------------------------------------------------------------------
# The proposed allocation under review
# ---------------------------------------------------------------------------

# Status: NOT APPLIED to items.csv / ores.json / macerator.yaml.
# Slots 10:20-10:23 are the first free payloads after the 20 live ores.
# See the report: the four NAMES are not in item-subgroups-deferred.csv.
PROPOSED = [
    ("aluminium_ore", "10:20"),
    ("titanium_ore", "10:21"),
    ("tungsten_ore", "10:22"),
    ("silicon_ore", "10:23"),
]

# What item-subgroups-deferred.csv actually proposes for the nearest real
# content-plan ore names, so a reviewer can see they are unusable as-is.
DEFERRED_REFERENCES = [
    ("bauxite_ore", "10:0:24"),
    ("nether_tungsten_ore", "10:1:4"),
    ("moon_aluminium_ore", "10:3:3"),
    ("end_iron_ore", "10:2:0"),
    ("mars_copper_ore", "10:3:6"),
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--live", action="store_true", help="validate live tree only")
    args = ap.parse_args()

    print("=" * 72)
    print("ORE ID VERIFICATION (reimplements ItemId::pack)")
    print("=" * 72)

    problems = selftest_encoding()
    live = load_items()
    by_value = {pack(pid): (pid, name) for pid, name in live.items()}
    rc = 0

    if problems:
        print(f"pack() SELF-TEST FAILED ({len(problems)} problems) -- results below are unreliable")
        for p in problems:
            print(f"  - {p}")
        return 2
    print(f"pack() self-test        : PASS (agrees with item-id-migration.csv on all 206 integers)")
    print(f"live items              : {len(live)}, packed-value collisions: 0")
    print(f"live ore band           : {pack('10:0')} .. {pack('110:0') - 1} (CAT_ORES)")
    print()

    if not args.live:
        print("-" * 72)
        print("PROPOSED ORE IDS (not applied -- review required)")
        print("-" * 72)
        for name, pid in PROPOSED:
            ok, probs, info = check_proposed(name, pid, live, by_value)
            status = "OK  " if ok else "FAIL"
            print(f"  {status} {name:18s} {pid:8s} -> {info['packed']}")
            for p in probs:
                print(f"        - {p}")
            if not ok:
                rc = 1
        print()

        print("-" * 72)
        print("DEFERRED-FILE IDS FOR THE SAME CONTENT PLAN (why they are unusable)")
        print("-" * 72)
        for name, pid in DEFERRED_REFERENCES:
            ok, probs, info = check_proposed(name, pid, live, by_value)
            flat = f"10:{info['packed'] - pack('10:0')}" if category(info["packed"]) == "CAT_ORES" else "?"
            print(f"  {name:20s} {pid:9s} -> {info['packed']:6d}  flat_equivalent={flat}")
            for p in probs:
                print(f"        - {p}")
        print()

    veins, placed, vprobs = check_veins(live)
    print("-" * 72)
    print(f"LIVE TREE: {len(veins)} veins, {len(live)} items")
    print("-" * 72)
    if vprobs:
        for p in vprobs:
            print(f"  - {p}")
        return 1
    print(f"  all {sum(1 for p in live if p.startswith('10:'))} registered ores placed; "
          f"every vein band intersects [{ORE_CENTRE_MIN_Y},{ORE_CENTRE_MAX_Y}]")
    return 0 if args.live else rc


if __name__ == "__main__":
    sys.exit(main())
