#!/usr/bin/env python3
"""
Convert legacy JSON recipe files to YAML format.

Maps by filename → machine class (ignores the 'm' field in JSON,
which often has wrong/stale block_ids). Outputs one .yaml per class.
"""

import json
import os
import sys
from pathlib import Path
from collections import OrderedDict

# ── Filename → machine class mapping ─────────────────────────────────────
FILENAME_TO_CLASS = {
    "furnace":              "furnace",
    "macerator":            "macerator",
    "steam_macerator":      "macerator",      # same class, different variant
    "steam_compressor":     "compressor",
    "bronze_alloy_smelter": "alloy_smelter",
    "steam_extractor":      "extractor",
    "steam_mixer":          "mixer",
    "assembler":            "assembler",
    "chemical_reactor":     "chemical_reactor",
    "crystallizer":         "crystallizer",
    "electrolyser":         "electrolyser",
    "crafting_table":       None,              # not a machine class — kept as legacy
}

# Classes that already have a YAML file (don't convert)
ALREADY_CONVERTED = {"macerator"}  # macerator.yaml already done manually

DATA_DIR = Path(__file__).resolve().parent.parent / "data"
RECIPES_DIR = DATA_DIR / "recipes"


def parse_json_item(entry):
    """Parse a JSON input entry: [item_id, count?] or [item_id, count?, {consume: false}]"""
    if not isinstance(entry, list) or len(entry) == 0:
        return None

    item_id = entry[0]
    if not isinstance(item_id, int) or item_id == 0:
        return None  # skip empty slots

    count = 1
    consume = True

    if len(entry) >= 2:
        if isinstance(entry[1], int):
            count = entry[1]

    if len(entry) >= 3 and isinstance(entry[2], dict):
        consume = entry[2].get("consume", True)

    result = OrderedDict()
    result["item"] = item_id
    result["count"] = count
    if not consume:
        result["consume"] = False
    return result


def parse_json_output(entry):
    """Parse a JSON output entry: [item_id] or [item_id, count]"""
    if not isinstance(entry, list) or len(entry) == 0:
        return None
    item_id = entry[0]
    if not isinstance(item_id, int) or item_id == 0:
        return None
    count = entry[1] if len(entry) >= 2 and isinstance(entry[1], int) else 1
    return {"item": item_id, "count": count}


def convert_recipe(name, data):
    """Convert a single JSON recipe dict to a YAML recipe OrderedDict."""
    recipe = OrderedDict()
    recipe["name"] = name

    # Inputs — filter out empty slots
    raw_inputs = data.get("in", [])
    inputs = []
    for entry in raw_inputs:
        parsed = parse_json_item(entry)
        if parsed is not None:
            inputs.append(parsed)
    if inputs:
        recipe["inputs"] = inputs

    # Outputs
    raw_outputs = data.get("out", [])
    outputs = []
    for entry in raw_outputs:
        parsed = parse_json_output(entry)
        if parsed is not None:
            outputs.append(parsed)
    if outputs:
        recipe["outputs"] = outputs
    else:
        recipe["outputs"] = []  # explicit (producer recipes with only energy_output)

    # Duration
    recipe["duration"] = data.get("dur", 200)

    # Energy cost (optional)
    eu = data.get("eu")
    if eu is not None:
        recipe["eu"] = eu

    # Tier bounds — legacy recipes are available to all variants
    recipe["min_tier"] = 0
    recipe["max_tier"] = 32767

    return recipe


def write_yaml_recipes(filepath, class_name, recipes):
    """Write a list of recipe dicts to a YAML file."""
    lines = []
    lines.append(f"# GTNH Platform — {class_name} recipes")
    lines.append(f"# Auto-converted from JSON")
    lines.append(f"class: {class_name}")
    lines.append("recipes:")

    for r in recipes:
        lines.append("")
        lines.append(f"  - name: {r['name']}")

        # Inputs
        if r.get("inputs"):
            lines.append("    inputs:")
            for inp in r["inputs"]:
                parts = [f"item: {inp['item']}", f"count: {inp['count']}"]
                if not inp.get("consume", True):
                    parts.append("consume: false")
                lines.append(f"      - {{ {', '.join(parts)} }}")

        # Outputs
        if r.get("outputs"):
            if len(r["outputs"]) == 0:
                lines.append("    outputs: []")
            else:
                lines.append("    outputs:")
                for out in r["outputs"]:
                    lines.append(f"      - {{ item: {out['item']}, count: {out['count']} }}")

        # Duration
        lines.append(f"    duration: {r['duration']}")

        # Energy cost
        if "eu" in r:
            # Format float nicely
            eu_val = r["eu"]
            if eu_val == int(eu_val):
                lines.append(f"    eu: {int(eu_val)}")
            else:
                lines.append(f"    eu: {eu_val}")

        # Tier bounds
        lines.append(f"    min_tier: {r['min_tier']}")
        lines.append(f"    max_tier: {r['max_tier']}")

    with open(filepath, "w") as f:
        f.write("\n".join(lines) + "\n")


def convert_file(json_path):
    """Convert a single JSON recipe file to YAML."""
    stem = json_path.stem  # e.g. "furnace" from "furnace.json"

    class_name = FILENAME_TO_CLASS.get(stem)
    if class_name is None:
        print(f"  ⏭️  {stem}.json → no class mapping (legacy)")
        return

    if stem in ALREADY_CONVERTED:
        print(f"  ⏭️  {stem}.json → already has YAML")
        return

    with open(json_path) as f:
        data = json.load(f)

    recipes = []
    for recipe_id, recipe_data in data.items():
        recipe = convert_recipe(recipe_id, recipe_data)
        recipes.append(recipe)

    if not recipes:
        print(f"  ⚠️  {stem}.json → no recipes found")
        return

    # Sort by name for determinism
    recipes.sort(key=lambda r: r["name"])

    # Write YAML
    yaml_path = json_path.with_suffix(".yaml")
    write_yaml_recipes(yaml_path, class_name, recipes)
    print(f"  ✅ {stem}.json → {yaml_path.name} ({len(recipes)} recipes, class={class_name})")


def main():
    os.makedirs(RECIPES_DIR, exist_ok=True)

    json_files = sorted(RECIPES_DIR.glob("*.json"))
    if not json_files:
        print("No JSON recipe files found.")
        return

    print(f"Converting {len(json_files)} JSON recipe files...\n")

    for jf in json_files:
        convert_file(jf)

    print("\nDone.")


if __name__ == "__main__":
    main()
