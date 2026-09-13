#!/usr/bin/env python3
"""Regenerate data/registry/item-subgroups-deferred.csv from todo_items/issue21.md.

Parses the two kinds of deferred-item provenance in issue21.md:
- proposed-registry table rows: ``<binary prefix>,<name>,<stack>,<meta>``
- prose item lists under section headers (e.g. "Circuits (need new sub-prefix)")

Table-row proposals that use the pre-migration pool notation (01110:X,
1110:00, 1110:01, 1110:10, 1110:11) are remapped to the new child prefixes
from data/registry/item-subgroups.json. Names already registered in
items.csv are skipped. Output rows are proposals only; final IDs still
require review per manifest policy.
"""
from __future__ import annotations

import csv
import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ISSUE21 = REPO_ROOT / "todo_items" / "issue21.md"
ITEMS_CSV = REPO_ROOT / "data" / "registry" / "items.csv"
OUTPUT = REPO_ROOT / "data" / "registry" / "item-subgroups-deferred.csv"

# Old proposal prefix -> new child prefix (post item-id renumbering).
REMAP_EXACT = {
    "01110:0": "0:1110:000",
    "01110:1": "0:1110:001",
    "01110:2": "0:1110:010",
    "01110:3": "0:1110:011",
    "01110:4": "0:1110:100",
    "01110:5": "0:1110:101",
    "1110:00": "1110:000",
    "1110:01": "1110:001",
    "1110:10": "1110:010",
}

STOPWORDS = {
    "items", "item", "recipe", "recipes", "yaml", "files", "file", "used",
    "ids", "use", "id", "csv", "prefix", "stack", "true", "false", "the",
    "and", "not", "in", "etc", "more", "done", "currently", "need", "needs",
    "room", "for", "per", "energy", "tier", "tiers", "consider", "allow",
    "allows", "have", "has", "also", "collisions", "implicit", "undocumented",
    "backward", "compat", "keeping", "migrate", "add", "either", "action",
    "gaps", "found", "sequential", "numeric", "dont", "match", "notation",
    "strings", "mapping", "old", "new", "review", "reviewed", "column",
    "updated", "entries", "corresponding", "matching", "acceptance",
    "criteria", "cables", "pipes", "fluids", "with", "corresponding",
    "default", "additions", "scheme", "from", "into", "that", "this",
    "unspecified", "cross", "cutting", "cheap", "manual", "only", "are",
    "was", "were", "all", "any", "some", "they", "them", "their", "its",
    "vanilla", "minecraft", "priority", "minimal", "needed", "current",
    "game", "trash", "other", "clean", "lsp", "diagnostics", "changed",
    "machines", "machine", "infrastructure", "components", "component",
    "circuits", "tools", "ores", "dusts", "plates", "wires", "gems",
    "sub", "parts", "fluid", "water", "cutting", "annealed", "generic",
}

TABLE_ROW = re.compile(r"^([0-9][0-9:]*)\s*,\s*([a-z][a-z0-9_]*)\s*,")
TOKEN = re.compile(r"\b([a-z][a-z0-9_]{2,})\b")
# Section header: "Name (something)" or a Title line ending with ':'.
HEADER = re.compile(r"^\s*([A-Z][^\n]*?)\s*(?:\(([^)]*)\))?\s*:?\s*$")


def registered_names() -> set:
    names = set()
    with open(ITEMS_CSV, newline="", encoding="utf-8") as handle:
        for row in csv.reader(handle):
            if row and not row[0].startswith("#") and row[0] != "int" and len(row) >= 2:
                names.add(row[1])
    return names


def remap_proposal(prefix: str, payload: int, name: str = "") -> str:
    if prefix in REMAP_EXACT:
        return f"{REMAP_EXACT[prefix]}:{payload}"
    if prefix == "1110:11":  # old combined generation/storage/transformers section
        if payload >= 40 or name.startswith(("casing_", "input_", "output_", "energy_hatch",
                                             "dynamo_hatch", "maintenance_hatch", "muffler_hatch",
                                             "multi_controller_")):
            child = "1110:111"
        elif name.startswith("transformer_"):
            child = "1110:110"
        elif name.startswith(("battery_", "charger", "energy_crystal", "lapotron",
                              "lapotronic")):
            child = "1110:101"
        else:
            child = "1110:100"
        return f"{child}:{payload}"
    return f"{prefix}:{payload}"


def main() -> None:
    registered = registered_names()
    rows: dict = {}

    def emit(name: str, line_no: int, label: str, proposed: str, reason: str) -> None:
        if name in registered or name in STOPWORDS:
            return
        row = rows.setdefault(name, {
            "name": name,
            "source_rows": [],
            "source_labels": label,
            "proposed_ids": proposed,
            "status": "deferred",
            "reason": reason,
        })
        if proposed and not row["proposed_ids"]:
            row["proposed_ids"] = proposed
        if line_no not in row["source_rows"]:
            row["source_rows"].append(line_no)

    header_label = ""
    header_active = False  # True while under an item-listing prose header
    lines = ISSUE21.read_text(encoding="utf-8").splitlines()
    for line_no, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#") or line.startswith("|") or line.startswith("-"):
            continue
        table = TABLE_ROW.match(line)
        if table and header_label:
            prefix, name = table.group(1), table.group(2)
            payload = int(prefix.rsplit(":", 1)[1])
            parent = prefix.rsplit(":", 1)[0] if ":" in prefix else ""
            proposed = remap_proposal(parent, payload, name) if parent else prefix
            emit(name, line_no, header_label, proposed,
                 "not-yet-reviewed-or-namespace-conflict")
            continue
        header = HEADER.match(line)
        # Headers may contain commas ("Generation, Storage, Transformers (1110:11)")
        # but prose list lines with a colon before any paren are not headers.
        pre_paren = line.split("(", 1)[0]
        if (header and not line.startswith("-") and len(line) < 90
                and ("," not in line or ("(" in line and ":" not in pre_paren))):
            label = header.group(1).strip()
            paren = (header.group(2) or "").strip()
            header_label = f"{label} ({paren})" if paren else label
            # Prose list headers carry a non-binary paren or none at all;
            # pure-binary parens are proposed-registry table sections.
            # Paren-less provenance headers are whitelisted explicitly.
            if re.fullmatch(r"[01]+(?::\*)?(?:\s*—.*)?", paren or ""):
                header_active = False
            elif paren:
                header_active = True
            else:
                header_active = label in {
                    "Processed materials", "Mechanical components",
                } or label.lower().startswith("items used in recipes")
            continue
        if header_active:
            if "need new sub-prefix" in header_label:
                reason = "requires-new-namespace"
            elif header_label.startswith("Decorative"):
                reason = "low-priority-vanilla"
            else:
                reason = "not-yet-reviewed-or-namespace-conflict"
            for token in TOKEN.findall(line):
                emit(token, line_no, header_label, "", reason)

    with open(OUTPUT, "w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(["name", "source_rows", "source_labels", "proposed_ids", "status", "reason"])
        for name in sorted(rows):
            row = rows[name]
            writer.writerow([
                row["name"],
                ";".join(str(n) for n in row["source_rows"]),
                row["source_labels"],
                row["proposed_ids"],
                row["status"],
                row["reason"],
            ])
    print(f"wrote {len(rows)} deferred rows to {OUTPUT}")


if __name__ == "__main__":
    main()
