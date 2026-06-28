"""Shared helpers — item name resolution, data directory discovery."""

from __future__ import annotations

import csv
import os
from typing import Optional

_ITEM_NAMES: dict[int, str] = {}       # id → name
_ITEM_IDS: dict[str, int] = {}         # name → id
_LOADED = False


def _discover_project_root() -> str:
    """Walk up from this file until we find data/registry/items.csv."""
    here = os.path.dirname(os.path.abspath(__file__))
    for _ in range(10):
        candidate = os.path.join(here, "data", "registry", "items.csv")
        if os.path.isfile(candidate):
            return here
        parent = os.path.dirname(here)
        if parent == here:
            break
        here = parent
    return os.path.dirname(os.path.abspath(__file__))


PROJECT_ROOT = _discover_project_root()
DATA_REGISTRY = os.path.join(PROJECT_ROOT, "data", "registry")
DATA_RECIPES = os.path.join(PROJECT_ROOT, "data", "recipes")


def load_item_names(path: Optional[str] = None) -> None:
    """Populate item name maps from items.csv."""
    global _ITEM_NAMES, _ITEM_IDS, _LOADED
    if _LOADED:
        return
    path = path or os.path.join(DATA_REGISTRY, "items.csv")
    if not os.path.isfile(path):
        return
    with open(path, newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        for row in reader:
            if not row:
                continue
            raw_id = row[0].strip()
            if not raw_id.isdigit():
                continue
            item_id = int(raw_id)
            name = row[1].strip() if len(row) > 1 else ""
            if name:
                _ITEM_NAMES[item_id] = name
                _ITEM_IDS[name] = item_id
    _LOADED = True


def item_name(item_id: int) -> str:
    """Return display name for an item ID, or '?{id}' if unknown."""
    load_item_names()
    return _ITEM_NAMES.get(item_id, f"?{item_id}")


def item_id(name: str) -> int:
    """Resolve item name → numeric ID. Returns 0 if unknown."""
    load_item_names()
    return _ITEM_IDS.get(name, 0)


def all_items() -> list[tuple[int, str]]:
    """Sorted list of (id, name) for the item picker."""
    load_item_names()
    return sorted(_ITEM_NAMES.items(), key=lambda x: x[1])


def resolve_item(value: str | int) -> int:
    """Accept '3', 'iron_ore', 3 → return numeric ID."""
    if isinstance(value, int):
        return value
    stripped = value.strip()
    if stripped.isdigit():
        return int(stripped)
    return item_id(stripped)
