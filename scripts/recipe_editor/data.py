"""Load and save YAML/CSV data files."""

from __future__ import annotations

import os
import yaml
from typing import Optional

from .models import MachineClass, MachineVariant, Recipe, Ingredient, OutputItem
from .utils import DATA_REGISTRY, DATA_RECIPES


# ── Machines ────────────────────────────────────────────────────────────────

def load_machines(path: Optional[str] = None) -> dict[str, MachineClass]:
    """Parse machines.yaml → dict of class name → MachineClass."""
    path = path or os.path.join(DATA_REGISTRY, "machines.yaml")
    if not os.path.isfile(path):
        return {}

    with open(path, encoding="utf-8") as f:
        root = yaml.safe_load(f)

    classes: dict[str, MachineClass] = {}
    for entry in root.get("machine_classes", []):
        mc = MachineClass(name=entry["class"])
        for v in entry.get("variants", []):
            mv = MachineVariant(
                block_id=v.get("block_id", 0),
                name=v.get("name", ""),
                energy_in=v.get("energy_in"),
                energy_out=v.get("energy_out"),
                tier=v.get("tier", 0),
                role=v.get("role", "consumer"),
                slots=v.get("slots"),
                energy=v.get("energy"),
                multiplier=v.get("multiplier"),
            )
            mc.variants.append(mv)
        classes[mc.name] = mc
    return classes


# ── Recipes ─────────────────────────────────────────────────────────────────

def load_recipes(directory: Optional[str] = None) -> dict[str, list[Recipe]]:
    """Load all .yaml recipe files → dict of class name → recipe list."""
    directory = directory or DATA_RECIPES
    if not os.path.isdir(directory):
        return {}

    by_class: dict[str, list[Recipe]] = {}
    for fname in sorted(os.listdir(directory)):
        if not fname.endswith((".yaml", ".yml")):
            continue
        fpath = os.path.join(directory, fname)
        with open(fpath, encoding="utf-8") as f:
            root = yaml.safe_load(f)
        if not root or not isinstance(root, dict):
            continue

        default_class = root.get("class", "")
        for entry in root.get("recipes", []):
            recipe = _parse_recipe(entry, default_class)
            if recipe is None:
                continue
            by_class.setdefault(recipe.machine_class, []).append(recipe)
    return by_class


def _parse_recipe(entry: dict, default_class: str) -> Optional[Recipe]:
    if not isinstance(entry, dict):
        return None
    name = entry.get("name", "")
    if not name:
        return None

    cls = entry.get("class", default_class) or ""
    if not cls:
        return None

    recipe = Recipe(
        name=name,
        machine_class=cls,
        duration=entry.get("duration", 200),
        energy_cost=float(entry.get("eu", 0)),
        energy_output=float(entry.get("energy_output", 0)),
        min_tier=int(entry.get("min_tier", 0)),
        max_tier=int(entry.get("max_tier", 32767)),
        energy_type=entry.get("energy_in"),
    )

    for inp in entry.get("inputs", []):
        recipe.inputs.append(Ingredient(
            item_id=int(inp.get("item", 0)),
            count=int(inp.get("count", 1)),
            consume=bool(inp.get("consume", True)),
            replace_item=int(inp.get("replace", 0)),
            replace_meta=int(inp.get("replace_meta", 0)),
        ))

    for out in entry.get("outputs", []):
        recipe.outputs.append(OutputItem(
            item_id=int(out.get("item", 0)),
            count=int(out.get("count", 1)),
            metadata=int(out.get("meta", 0)),
            display_name=out.get("display_name"),
            color=out.get("color"),
            unlocalized_name=out.get("unlocalized_name"),
            lore=out.get("lore"),
        ))

    return recipe


# ── Save ────────────────────────────────────────────────────────────────────

def save_recipes(
    by_class: dict[str, list[Recipe]],
    directory: Optional[str] = None,
    *,
    dry_run: bool = False,
) -> list[str]:
    """Write recipe files, one YAML per class. Returns written paths."""
    directory = directory or DATA_RECIPES
    os.makedirs(directory, exist_ok=True)

    written: list[str] = []
    for class_name in sorted(by_class):
        recipes = by_class[class_name]
        if not recipes:
            continue

        doc = {"class": class_name, "recipes": []}
        for r in recipes:
            entry = {"name": r.name}
            if r.energy_type is not None:
                entry["energy_in"] = r.energy_type
            if r.min_tier != 0:
                entry["min_tier"] = r.min_tier
            if r.max_tier != 32767:
                entry["max_tier"] = r.max_tier

            if r.inputs:
                entry["inputs"] = [
                    _ingredient_yaml(i) for i in r.inputs
                ]
            entry["outputs"] = [
                _output_yaml(o) for o in r.outputs
            ]
            entry["duration"] = r.duration
            if r.energy_cost:
                entry["eu"] = r.energy_cost
            if r.energy_output:
                entry["energy_output"] = r.energy_output

            doc["recipes"].append(entry)

        fname = f"{class_name}.yaml"
        fpath = os.path.join(directory, fname)
        if not dry_run:
            with open(fpath, "w", encoding="utf-8") as f:
                yaml.dump(doc, f, default_flow_style=None,
                          allow_unicode=True, sort_keys=False)
        written.append(fpath)
    return written


def _ingredient_yaml(i: Ingredient) -> dict:
    d: dict = {"item": i.item_id, "count": i.count}
    if not i.consume:
        d["consume"] = False
    if i.replace_item:
        d["replace"] = i.replace_item
    if i.replace_meta:
        d["replace_meta"] = i.replace_meta
    return d


def _output_yaml(o: OutputItem) -> dict:
    d: dict = {"item": o.item_id, "count": o.count}
    if o.metadata:
        d["meta"] = o.metadata
    if o.display_name is not None:
        d["display_name"] = o.display_name
    if o.color is not None:
        d["color"] = o.color
    if o.unlocalized_name is not None:
        d["unlocalized_name"] = o.unlocalized_name
    if o.lore:
        d["lore"] = o.lore
    return d
