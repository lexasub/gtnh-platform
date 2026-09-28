#!/usr/bin/env python3
"""Content reachability audit for the GTNH crafting graph.

Answers: which registry items can a player actually obtain, and which are
unreachable, and why. Pure stdlib + PyYAML, no build required.

It deliberately mirrors the RUNTIME's acceptance rules rather than being
stricter or looser, because a stricter parse invents recipes that do not
exist at runtime and hides real gaps:

  * RecipeManager::loadRecipesFromYamlDirectory (RecipeManager.cpp:532-540)
    only loads files whose extension is .yaml / .yml -- mixer.yaml.bak and
    compressor.yaml.bak are NOT loaded.
  * RecipeManager::loadRecipesFromYamlFile (:516) requires a 'recipes' key.
  * RecipeManager::parseYamlRecipe (:617) requires 'name'.
  * ...:627 rejects a recipe with no machine class (no per-recipe 'class'
    and no file-level default).
  * ...:701-714 REJECTS any recipe with no 'outputs' key, unless the
    machine class is 'generator' or 'boiler' (they yield energy, not items).
    This is the rule that makes the ~27 name-only stubs non-recipes.
  * ...:802-812 rejects a recipe whose resource_requirements' kind
    contradicts an explicit energy_in declaration.
  * RecipeManager.cpp:952-991 parses the per-operation fluid port
    (`fluid_inputs` / `fluid_outputs`, RecipeTypes.h:52 FluidIOItem) and is
    STRICT: the entry must carry a `fluid` key naming a registered fluid
    (ItemId::isFluid) and an `amount` > 0, or the WHOLE recipe is rejected --
    the entry is never quietly dropped.

An item is CRAFTABLE if AT LEAST ONE recipe producing it has every input
already craftable. The fixpoint starts from what the world yields with zero
crafting: the terrain blocks named in WorldGenerator.cpp:22-26, the
block->drop mappings in drops.csv, oak_log from TreeGenerator.h:24, and
every block named in ores.json veins.

The fluid port is part of that input/output set, not a separate graph: a
`fluid_inputs` entry is a required input exactly like an item input, and a
`fluid_outputs` entry is a producer exactly like an item output. That is what
makes a fluid obtainable once some recipe really makes it. A fluid with no
producer classifies as a progression root, by the same root/orphan/cascade
rule items use.

Usage:
    python3 tools/reachability_audit.py                # summary
    python3 tools/reachability_audit.py --roots        # unreachable roots
    python3 tools/reachability_audit.py --check item[,item...]  # gate
    python3 tools/reachability_audit.py --json         # machine readable
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import sys
from collections import defaultdict

import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGISTRY = os.path.join(ROOT, "src/content/data/registry")
RECIPES = os.path.join(ROOT, "src/content/data/recipes")

# Machine classes that legitimately have no 'outputs': they produce energy
# (energy_output), not items. RecipeManager.cpp:700.
ENERGY_PRODUCERS = {"generator", "boiler"}

# ItemId::isFluid (ItemId.h:193) is a range test against this packed id. Named
# here so the fluid port's acceptance rule has one definition shared by
# is_fluid_ref and the report, instead of a magic 0xFC00 in two places.
# "1111:11:0" -> prefix bits 1111 + 11 = 6 bits, shifted left by 16-6=10.
FLUID_ID_MIN = 0b111111 << 10  # == 0xFC00 == 64512

# OreGenerator.cpp:13 -- thread_local yDist(5, 60) picks the vein centre y.
# A vein outside this range is never selected, so it is dead config.
ORE_CENTRE_MIN_Y = 5
ORE_CENTRE_MAX_Y = 60

# ---------------------------------------------------------------------------
# Registry
# ---------------------------------------------------------------------------


def load_items(path: str) -> dict[str, str]:
    """packed_id -> name, skipping '#' comment rows and the header."""
    items: dict[str, str] = {}
    with open(path, newline="", encoding="utf-8") as fh:
        for row in csv.reader(fh):
            if not row:
                continue
            first = row[0].strip()
            # Comment rows and the header are not items.
            if not first or first.startswith("#") or first == "int":
                continue
            if len(row) < 2:
                continue
            name = row[1].strip()
            if not name:
                continue
            items[first] = name
    return items


def load_name_index(items: dict[str, str]) -> dict[str, str]:
    return {name: pid for pid, name in items.items()}


# ---------------------------------------------------------------------------
# Recipes
# ---------------------------------------------------------------------------


def _resolve(ref, name_to_id) -> str | None:
    """Map a recipe-side item reference to a packed id, or None if unknown."""
    if ref is None:
        return None
    ref = str(ref).strip()
    if not ref or ref == "~":
        return None
    # resolveItemId: ':' or all-digits -> packed id, else registry name.
    if ":" in ref or ref.isdigit():
        return ref
    return name_to_id.get(ref)


def is_fluid_ref(ref: str) -> bool:
    """ItemId::isFluid — a pure RANGE test, not a fluids.csv lookup.

    ItemId.h:193 is `id >= ItemId::pack("1111:11:0")`, i.e. 0xFC00 and up. It
    deliberately does NOT consult fluids.csv, so ids in that band that are in
    items.csv but absent from fluids.csv (ethylene 1111:11:6, polyethylene
    1111:11:7) DO satisfy the runtime's fluid test. Mirroring fluids.csv here
    instead would make this audit reject recipes the server accepts, which is
    exactly the "stricter parse invents rules" failure this file exists to
    avoid.
    """
    return _pack(ref) >= FLUID_ID_MIN


def _pack(ref: str) -> int:
    """ItemId::pack: prefix bits shifted to the top, payload OR'd in.

    Only used to answer RANGE questions (is this id a fluid). Reachability
    itself is tracked with the string ids the rest of this file already uses,
    so this never becomes a second id namespace.
    """
    if not ref:
        return 0
    last = ref.rfind(":")
    if last < 0:
        value = 0
        for c in ref:
            if c.isdigit():
                value = value * 10 + int(c)
        return value & 0xFFFF
    prefix = 0
    plen = 0
    for c in ref[:last]:
        if c == "0":
            prefix <<= 1
            plen += 1
        elif c == "1":
            prefix = (prefix << 1) | 1
            plen += 1
    if plen > 15:
        return 0  # too many prefix bits -- pack() bails, same as C++
    payload = 0
    for c in ref[last + 1 :]:
        if c.isdigit():
            payload = payload * 10 + int(c)
    return ((prefix << (16 - plen)) | payload) & 0xFFFF


def resolve_fluid_io(entry, name_to_id):
    """Resolve one `fluid_inputs`/`fluid_outputs` entry to a packed id.

    Mirrors parseFluidIOList (RecipeManager.cpp:952-987) exactly, including the
    three rejection conditions: no `fluid` key, a name that does not resolve to
    a registered fluid, and `amount` <= 0. Returns (packed_id, reason). A
    non-empty reason means the runtime REJECTS THE WHOLE RECIPE -- not that the
    entry is skipped -- so the caller must reject the recipe, never drop the
    entry. Dropping it would silently delete a requirement the server enforces
    and would report the recipe as craftable when the server refuses to run it.
    """
    if not isinstance(entry, dict):
        return None, "entry is not a mapping"
    name = entry.get("fluid")
    if name is None or not str(name).strip():
        return None, "entry has no 'fluid'"
    ref = _resolve(name, name_to_id)
    if not ref:
        return None, f"fluid '{name}' is not a registered item"
    if not is_fluid_ref(ref):
        return None, f"'{name}' ({ref}) is not a fluid id (ItemId::isFluid)"
    # `amount` must be > 0. The runtime reads it as uint32_t, so a missing or
    # unparsable value is 0 -- which is the reject case, not a default.
    try:
        amount = int(entry.get("amount", 0) or 0)
    except (TypeError, ValueError):
        amount = 0
    if amount <= 0:
        return None, f"fluid '{name}' has amount {entry.get('amount')!r}, must be > 0"
    return ref, None


def parse_recipe_files(items_by_id, name_to_id):
    """Return (live_recipes, rejected) mirroring the runtime loader."""
    live = []
    rejected = []

    files = sorted(
        f
        for f in os.listdir(RECIPES)
        # RecipeManager.cpp:538 -- .yaml / .yml only. A .bak is dead data.
        if os.path.isfile(os.path.join(RECIPES, f)) and f.endswith((".yaml", ".yml"))
    )

    for fname in files:
        path = os.path.join(RECIPES, fname)
        with open(path, encoding="utf-8") as fh:
            try:
                root = yaml.safe_load(fh)
            except yaml.YAMLError as exc:
                rejected.append((fname, "<file>", f"YAML parse error: {exc}"))
                continue

        if not isinstance(root, dict) or "recipes" not in root:
            # RecipeManager.cpp:516
            rejected.append((fname, "<file>", "no 'recipes' section"))
            continue

        # File-level default class (RecipeManager.cpp:510-514).
        default_class = root.get("class") or ""

        for entry in root.get("recipes") or []:
            if not isinstance(entry, dict):
                continue
            rid = entry.get("name")
            if not rid:
                # RecipeManager.cpp:617
                rejected.append((fname, "<unnamed>", "missing 'name'"))
                continue

            machine_class = entry.get("class") or default_class
            if not machine_class:
                # RecipeManager.cpp:627
                rejected.append((fname, rid, "no machine class"))
                continue

            # THE rule this audit exists for: RecipeManager.cpp:701-714.
            outputs = entry.get("outputs")
            if machine_class not in ENERGY_PRODUCERS:
                if not isinstance(outputs, list):
                    rejected.append((fname, rid, "missing or invalid 'outputs'"))
                    continue
                if not outputs:
                    rejected.append((fname, rid, "empty outputs"))
                    continue

            # RecipeManager.cpp:767 -- a requirement tier outside the
            # recipe's own tier range is a hard reject.
            min_tier = entry.get("min_tier", 0)
            max_tier = entry.get("max_tier", 32767)
            reqs = (
                entry.get("resource_requirements")
                or entry.get("resources")
                or entry.get("requirements")
            )
            if isinstance(reqs, list):
                bad = False
                for req in reqs:
                    if not isinstance(req, dict):
                        bad = True
                        break
                    tier = req.get("tier", 0)
                    if not (min_tier <= tier <= max_tier):
                        bad = True
                        break
                if bad:
                    rejected.append((fname, rid, "requirement tier outside recipe tier range"))
                    continue

            # RecipeManager.cpp:804-812 -- an explicit resource kind that
            # contradicts an explicit energy_in can never be served.
            energy_in = entry.get("energy_in")
            kind_to_energy = {"EU": "ELECTRICITY", "HU": "HEAT", "STEAM": "STEAM", "RU": "ROTATION"}
            if energy_in and isinstance(reqs, list):
                first_req = reqs[0] if reqs and isinstance(reqs[0], dict) else {}
                declared = kind_to_energy.get(str(first_req.get("kind", "")))
                if declared and declared != energy_in:
                    rejected.append((fname, rid, f"resource kind contradicts energy_in {energy_in}"))
                    continue

            def resolve_list(seq):
                """Resolve a sequence of item refs to packed ids.

                An unresolvable name is dropped, mirroring the runtime where
                resolveItemName returns 0 for an unknown name and the input is
                then skipped (RecipeManager.cpp:659 requires an explicit
                'item' key to accept a zero id).
                """
                out = []
                if isinstance(seq, list):
                    for it in seq:
                        if isinstance(it, dict):
                            ref = _resolve(it.get("item"), name_to_id)
                        else:
                            ref = _resolve(it, name_to_id)
                        if ref:
                            out.append(ref)
                return out

            # Positional pattern (crafting table). RecipeManager.cpp:671-697
            # builds the cell list from the pattern; a cell may be a bare
            # name or '~'/null for empty. `inputs` is kept as aggregate
            # metadata for display, so the pattern is the real input set.
            pattern_cells: list[str] = []
            pat = entry.get("pattern")
            if pat is not None:
                rows = pat if (isinstance(pat, list) and len(pat) == 3) else [pat]
                for row in rows:
                    if isinstance(row, list):
                        for cell in row:
                            ref = _resolve(cell, name_to_id)
                            if ref:
                                pattern_cells.append(ref)

            # The per-operation fluid port (RecipeManager.cpp:952-991). This
            # is the same kind of gate as the rules above: a malformed entry
            # fails the WHOLE recipe, so it is checked BEFORE anything is
            # appended to `live`. Resolving the entries lazily here (rather
            # than in a separate pass) is deliberate: a recipe whose fluid
            # entry is bad must not be a producer or a consumer of anything.
            fluid_inputs: list[str] = []
            fluid_outputs: list[str] = []
            fluid_error = None
            for key, target in (
                ("fluid_inputs", fluid_inputs),
                ("fluid_outputs", fluid_outputs),
            ):
                seq = entry.get(key)
                if seq is None:
                    continue
                if not isinstance(seq, list):
                    fluid_error = f"{key} must be a sequence"
                    break
                for io_entry in seq:
                    ref, why = resolve_fluid_io(io_entry, name_to_id)
                    if why or ref is None:
                        # `ref is None` is unreachable when `why` is empty, but
                        # the assert-free form keeps the list typed `str`.
                        fluid_error = f"{key}: {why or 'unresolved fluid'}"
                        break
                    target.append(ref)
                if fluid_error:
                    break
            if fluid_error:
                rejected.append((fname, rid, fluid_error))
                continue

            inputs = pattern_cells or resolve_list(entry.get("inputs"))
            out_refs = resolve_list(outputs) if isinstance(outputs, list) else []

            live.append(
                {
                    "id": rid,
                    "file": fname,
                    "class": machine_class,
                    "inputs": inputs,
                    "outputs": out_refs,
                    "fluid_inputs": fluid_inputs,
                    "fluid_outputs": fluid_outputs,
                    "min_tier": min_tier,
                    "max_tier": max_tier,
                    "unlock_era": entry.get("unlock_era", 0),
                }
            )

    return live, rejected


# ---------------------------------------------------------------------------
# World base set
# ---------------------------------------------------------------------------


def load_drops(path: str) -> list[tuple[str, str]]:
    """source -> result block mappings from drops.csv."""
    pairs = []
    with open(path, newline="", encoding="utf-8") as fh:
        for row in csv.reader(fh):
            if not row:
                continue
            first = row[0].strip()
            if not first or first.startswith("#") or first == "source":
                continue
            if len(row) >= 2 and row[1].strip():
                pairs.append((first, row[1].strip()))
    return pairs


def load_ore_blocks(path: str) -> list[str]:
    """Every packed block id named by an ores.json vein."""
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    out = []
    for vein in data.get("veins", []):
        for key in ("primary", "secondary", "sporadic"):
            ref = vein.get(key)
            if ref:
                out.append(str(ref))
    return out


def load_fluid_ids() -> set[str]:
    """Every packed block id registered in fluids.csv."""
    path = os.path.join(REGISTRY, "fluids.csv")
    if not os.path.exists(path):
        return set()
    ids: set[str] = set()
    with open(path, newline="", encoding="utf-8") as fh:
        for row in csv.reader(fh):
            if not row:
                continue
            first = row[0].strip()
            if not first or first.startswith("#") or first == "item_id":
                continue
            ids.add(first)
    return ids


# Fluids that some earlier rule wanted to treat as obtainable but that
# drops.csv never actually yields. Reported so the number is visible instead of
# silently inflating the reachable count.
fluids_without_item_source: set[str] = set()


def world_base(items_by_id):
    """Items obtainable with zero crafting, and the reason for each."""
    base: dict[str, str] = {}

    # WorldGenerator.cpp:22-26 -- terrain blocks the generator places.
    for ref, why in (
        ("0:0:0", "WorldGenerator.cpp:22 BLOCK_AIR"),
        ("0:0:1", "WorldGenerator.cpp:23 BLOCK_STONE"),
        ("0:0:8", "WorldGenerator.cpp:24 BLOCK_GRASS"),
        ("0:0:7", "WorldGenerator.cpp:25 BLOCK_DIRT"),
        ("1111:11:0", "WorldGenerator.cpp:26 BLOCK_WATER"),
    ):
        if ref in items_by_id:
            base[ref] = why

    # TreeGenerator.h:24 BLOCK_LOG -- tree gen lives in its own module, so a
    # world_generator terrain-only grep misses it.
    oak = "0:10:11:2"
    if oak in items_by_id:
        base[oak] = "TreeGenerator.h:24 BLOCK_LOG"

    # drops.csv -- breaking a block yields its mapped drop.
    drops_path = os.path.join(REGISTRY, "drops.csv")
    if os.path.exists(drops_path):
        for src, dst in load_drops(drops_path):
            if dst in items_by_id:
                base.setdefault(dst, f"drops.csv {src}->{dst}")

    # ores.json -- every block a vein can place.
    for ref in load_ore_blocks(os.path.join(REGISTRY, "ores.json")):
        if ref in items_by_id:
            base.setdefault(ref, "ores.json vein")

    # fluids.csv -- a registered fluid is a BLOCK, not an inventory item.
    #
    # WorldGenerator.cpp:26 places BLOCK_WATER, which is why water used to be
    # added to the base set. That was wrong for every fluid: a water block in
    # the terrain is scenery, and there is no way to turn it into the item
    # `water` that recipes require. drops.csv is the only block -> item bridge
    # in the game, and it maps nothing to a fluid id.
    #
    # So a fluid counts as obtainable only when drops.csv actually yields it,
    # which is what the drops.csv loop above already established. Reaching this
    # point with a fluid in `base` means a recipe wants the block-as-item and
    # the game cannot supply it.
    for ref in load_fluid_ids():
        if ref in base and ref in items_by_id:
            del base[ref]
            fluids_without_item_source.add(ref)

    return base


# ---------------------------------------------------------------------------
# Fixpoint
# ---------------------------------------------------------------------------


def build_producers(live):
    """id -> list of producing recipes, over ITEM *and* FLUID outputs.

    A `fluid_outputs` entry makes its fluid a producer here, by the same rule an
    item output does. That is the entire point of the fluid port: a fluid with
    a real source has to become reachable, and it only becomes reachable if the
    fixpoint sees that source. Keyed in the same id space as everything else,
    so an item and a fluid cannot collide.
    """
    producers: dict[str, list] = defaultdict(list)
    for r in live:
        for out in r["outputs"]:
            producers[out].append(r)
        for out in r.get("fluid_outputs", ()):
            producers[out].append(r)
    return producers


def required_inputs(r):
    """Every id a recipe must be fed, items and fluids together.

    A `fluid_inputs` entry is a REQUIREMENT, not a hint: Recipe::needsReservation
    (RecipeTypes.h:151) makes the machine reserve the volume before the craft
    starts, so a recipe demanding an unobtainable fluid cannot be run. Folding
    these in here is what stops an unresolved fluid from being read as
    satisfied, which would report the recipe craftable when the server would
    refuse to start it.

    Pattern precedence is unchanged and needs no branch here: `r["inputs"]`
    is already `pattern_cells or resolve_list(inputs)` (see parse_recipe_files),
    so a positional recipe contributes its pattern and an aggregate recipe
    contributes its input list. The fluid port is ADDITIONAL either way.
    """
    return list(r["inputs"]) + list(r.get("fluid_inputs", ()))


def fixpoint(items_by_id, base, producers):
    """An item is craftable if AT LEAST ONE producer has all inputs ready."""
    craftable = {ref for ref in base if ref in items_by_id}

    changed = True
    rounds = 0
    while changed:
        changed = False
        rounds += 1
        for item, recs in producers.items():
            if item in craftable:
                continue
            for r in recs:
                if all(i in craftable for i in required_inputs(r)):
                    craftable.add(item)
                    changed = True
                    break

    unreachable = set(items_by_id) - craftable
    return craftable, unreachable, rounds


def classify(unreachable, producers, referenced):
    """Split the unreachable set by cause.

    Three buckets, because "no recipe" alone hides the actionable subset:

    * root    -- nothing produces it AND some live recipe consumes it, so it
                 is a true progression blocker. This is the fixable set.
    * orphan  -- nothing produces it AND nothing consumes it either: dead
                 content, unreachable but blocking nobody.
    * cascade -- a recipe exists but an input is unreachable.
    """
    roots, orphans, cascade = set(), set(), set()
    for item in unreachable:
        if producers.get(item):
            cascade.add(item)
        elif item in referenced:
            roots.add(item)
        else:
            orphans.add(item)
    return roots, orphans, cascade


def blocking_count(item, unreachable, producers, memo={}):
    """How many items are downstream-blocked by `item`, directly."""
    if item in memo:
        return memo[item]
    memo[item] = 0
    memo[item] = sum(
        1
        for other in unreachable
        if other != item
        and any(
            item in required_inputs(r) and other in r["outputs"] for r in producers.get(other, [])
        )
    )
    return memo[item]


def transitive_blocked(item, unreachable, producers):
    """All items transitively downstream of `item`."""
    seen = set()
    stack = [item]
    while stack:
        cur = stack.pop()
        for other, recs in producers.items():
            if other in seen or other not in unreachable:
                continue
            if any(cur in required_inputs(r) for r in recs):
                seen.add(other)
                stack.append(other)
    return seen


# ---------------------------------------------------------------------------


def check_veins(items_by_id, path=None):
    """Validate ores.json against the generator's actual constraints.

    Two invariants that a purely data-shaped check would miss:

    1. Every registered ore must be placed by at least one vein. A vein
       referencing an id absent from items.csv places a block no recipe or
       registry entry can name.
    2. A vein is only ever selected when min_y <= centre_y <= max_y, and
       OreGenerator.cpp:13 draws centre_y uniformly from [5, 60]. A vein
       whose band misses [5, 60] is dead config: it parses, loads, and never
       generates a single block.
    """
    path = path or os.path.join(REGISTRY, "ores.json")
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    veins = data.get("veins", [])

    required = {"name", "min_y", "max_y", "weight", "primary", "secondary", "sporadic"}
    problems = []
    placed: dict[str, list[str]] = {}

    for v in veins:
        missing = required - set(v)
        if missing:
            problems.append(f"vein {v.get('name')!r} missing field(s): {sorted(missing)}")
            continue
        for key in ("primary", "secondary", "sporadic"):
            ref = str(v[key])
            if ref not in items_by_id:
                problems.append(
                    f"vein {v['name']!r} {key}={ref} is not a registered item"
                )
            placed.setdefault(ref, []).append(f"{v['name']}.{key}")
        lo, hi = max(v["min_y"], ORE_CENTRE_MIN_Y), min(v["max_y"], ORE_CENTRE_MAX_Y)
        if lo > hi:
            problems.append(
                f"vein {v['name']!r} band [{v['min_y']},{v['max_y']}] never intersects the "
                f"generator's centre range [{ORE_CENTRE_MIN_Y},{ORE_CENTRE_MAX_Y}] - DEAD"
            )

    all_ores = {pid: name for pid, name in items_by_id.items() if pid.startswith("10:")}
    # Sort by the numeric ore index, not by name. Keep the pid alongside so
    # the sort key never has to reverse-look-up the name.
    unplaced = sorted(
        ((pid, name) for pid, name in all_ores.items() if pid not in placed),
        key=lambda kv: int(kv[0].split(":")[1]),
    )
    for _pid, name in unplaced:
        problems.append(f"registered ore {name} is placed by NO vein")

    return veins, placed, all_ores, problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--roots", action="store_true", help="list unreachable roots")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--check", metavar="ITEMS", help="comma-separated item names to gate")
    ap.add_argument(
        "--veins", action="store_true", help="validate ores.json coverage/band liveness"
    )
    args = ap.parse_args()

    items_by_id = load_items(os.path.join(REGISTRY, "items.csv"))
    live, rejected = parse_recipe_files(items_by_id, load_name_index(items_by_id))
    base = world_base(items_by_id)
    producers = build_producers(live)
    craftable, unreachable, rounds = fixpoint(items_by_id, base, producers)
    # `referenced` is the consumption test behind the root/orphan split, so it
    # has to see the fluid port too: a fluid with no producer but a live
    # `fluid_inputs` consumer is a progression root, the same as an item.
    # Using r["inputs"] here alone would report it as a harmless orphan and
    # hide the exact breakage the fluid port exists to expose.
    referenced = {i for r in live for i in required_inputs(r)}
    roots, orphans, cascade = classify(unreachable, producers, referenced)
    # How much of the content tree actually exercises the fluid port. Zero
    # means the port is parsed and enforced but unused by content -- the model
    # is still correct, but no content result above is attributable to it.
    recipes_with_fluid_io = [r for r in live if r["fluid_inputs"] or r["fluid_outputs"]]

    if args.veins:
        veins, placed, all_ores, problems = check_veins(items_by_id)
        print("=" * 68)
        print("ORE VEIN VALIDATION (ores.json)")
        print("=" * 68)
        print(f"veins: {len(veins)}   registered ores: {len(all_ores)}")
        print()
        for pid, name in sorted(all_ores.items(), key=lambda kv: int(kv[0].split(":")[1])):
            where = placed.get(pid, [])
            mark = "OK " if where else "!!!"
            print(f"  {mark} {pid:8s} {name:16s} {', '.join(where) if where else 'NOT PLACED'}")
        print()
        for v in veins:
            lo = max(v["min_y"], ORE_CENTRE_MIN_Y)
            hi = min(v["max_y"], ORE_CENTRE_MAX_Y)
            live = "live" if lo <= hi else "DEAD"
            print(
                f"  {v['name']:24s} band=[{v['min_y']},{v['max_y']}] "
                f"effective=[{lo},{hi}] w={v['weight']} {live}"
            )
        print()
        if problems:
            print(f"PROBLEMS ({len(problems)}):")
            for p in problems:
                print(f"  - {p}")
            return 1
        print("All ores placed by a vein; every vein band is reachable.")
        return 0

    if args.check:
        wanted = [x.strip() for x in args.check.split(",") if x.strip()]
        rev = {v: k for k, v in items_by_id.items()}
        rc = 0
        for name in wanted:
            ref = rev.get(name)
            if ref is None:
                print(f"  UNKNOWN  {name} (not in items.csv)")
                rc = 1
            elif ref in craftable:
                print(f"  OK       {name} ({ref}) is craftable")
            else:
                cause = (
                    "no producing recipe (progression root)"
                    if ref in roots
                    else "orphan: nothing produces or consumes it"
                    if ref in orphans
                    else "cascade (input unreachable)"
                )
                print(f"  BLOCKED  {name} ({ref}) - {cause}")
                rc = 1
        return rc

    if args.json:
        print(
            json.dumps(
                {
                    "total": len(items_by_id),
                    "craftable": len(craftable),
                    "unreachable": len(unreachable),
                    "roots": sorted(items_by_id[i] for i in roots),
                    "orphans": sorted(items_by_id[i] for i in orphans),
                    "cascade": sorted(items_by_id[i] for i in cascade),
                    "recipes_live": len(live),
                    "recipes_rejected": len(rejected),
                    "recipes_with_fluid_io": len(recipes_with_fluid_io),
                    "fluid_inputs_entries": sum(len(r["fluid_inputs"]) for r in live),
                    "fluid_outputs_entries": sum(len(r["fluid_outputs"]) for r in live),
                },
                indent=2,
            )
        )
        return 0

    print("=" * 68)
    print("CONTENT REACHABILITY AUDIT")
    print("=" * 68)
    print(f"registry items        : {len(items_by_id)}")
    print(f"recipes live          : {len(live)}   (rejected by runtime rules: {len(rejected)})")
    print(f"world base set        : {len(base)}")
    print(f"fixpoint rounds       : {rounds}")
    if fluids_without_item_source:
        names = sorted(
            items_by_id[ref] for ref in fluids_without_item_source if ref in items_by_id
        )
        print()
        print(f"  NOTE: {len(names)} fluid(s) are registered as items but the world never")
        print("  yields them as inventory items (no drops.csv mapping):")
        print("    " + ", ".join(names))
        print("  A fluid BLOCK in the terrain is not a craftable ITEM.")
    print()
    print(f"CRAFTABLE             : {len(craftable)}")
    print(f"UNREACHABLE           : {len(unreachable)}")
    print(f"  progression roots   : {len(roots)}   <- no producer, but a recipe needs it")
    print(f"  orphans             : {len(orphans)}   <- neither produced nor consumed")
    print(f"  pure cascade        : {len(cascade)}")
    print()
    print(f"fluid port in use     : {len(recipes_with_fluid_io)} recipe(s)")
    print(f"  fluid_inputs entries: {sum(len(r['fluid_inputs']) for r in live)}")
    print(f"  fluid_outputs       : {sum(len(r['fluid_outputs']) for r in live)}")
    print()

    if args.roots or args.verbose:
        print("-" * 68)
        print("UNREACHABLE ROOTS (nothing produces them at all)")
        print("-" * 68)
        rows = []
        for ref in roots:
            name = items_by_id[ref]
            down = len(transitive_blocked(ref, unreachable, producers))
            rows.append((down, name, ref))
        for down, name, ref in sorted(rows, reverse=True):
            print(f"  {down:4d} downstream  {name} ({ref})")
        print()

    if args.verbose and rejected:
        print("-" * 68)
        print(f"RECIPES REJECTED BY RUNTIME RULES ({len(rejected)})")
        print("-" * 68)
        for f, rid, why in rejected:
            print(f"  {f:32s} {rid:36s} {why}")
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
