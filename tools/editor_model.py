#!/usr/bin/env python3
import csv
import re
from collections import defaultdict
from typing import Dict, List, Tuple

Item = Tuple[str, str, str, str]


def parse_csv(path: str) -> Tuple[List[Item], Dict[str, List[Item]], Dict[str, str]]:
    items = []
    grouped = defaultdict(list)
    labels = {"": "root"}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                m = re.match(r"#\s*(.+?)\s*\(prefix\s+([\d:]+)\)", line)
                if m:
                    labels[m.group(2)] = m.group(1).strip()
                continue
            parts = line.split(",", 3)
            if len(parts) < 2:
                continue
            item_id = parts[0].strip()
            name = parts[1].strip()
            stack = parts[2].strip() if len(parts) > 2 else ""
            meta = parts[3].strip() if len(parts) > 3 else "0"
            items.append((item_id, name, stack, meta))
            segs = item_id.split(":")
            prefix = ":".join(segs[:-1]) if len(segs) > 1 else ""
            grouped[prefix].append((item_id, name, stack, meta))
    return items, dict(grouped), labels


def build_tree(grouped: Dict[str, List[Item]]) -> dict:
    tree = {}
    for prefix in sorted(grouped.keys()):
        node = tree
        for seg in prefix.split(":"):
            if seg not in node:
                node[seg] = {}
            node = node[seg]
        node["__items__"] = grouped[prefix]
    return tree


def subtree_count(node: dict) -> int:
    total = 0
    for key in node:
        if key == "__items__":
            total += len(node["__items__"])
        elif isinstance(node[key], dict):
            total += subtree_count(node[key])
    return total


def validate_ids(items: List[Item]) -> List[str]:
    seen = {}
    errors = []
    for item_id, name, _, _ in items:
        if item_id in seen:
            errors.append(f"Duplicate: {item_id} ({name} vs {seen[item_id]})")
        seen[item_id] = name
    return errors


def find_next_id(grouped: Dict[str, List[Item]], prefix: str) -> str:
    items = grouped.get(prefix, [])
    existing = set()
    for item_id, _, _, _ in items:
        segs = item_id.split(":")
        try:
            existing.add(int(segs[-1]))
        except (ValueError, IndexError):
            pass
    for i in range(1000):
        if i not in existing:
            return f"{prefix}:{i}" if prefix else str(i)
    return f"{prefix}:999" if prefix else "999"
