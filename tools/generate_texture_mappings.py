#!/usr/bin/env python3
"""Dry-run-by-default additive scanner for raw texture artwork."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
TEXTURES = ROOT / "data" / "textures"
RAW = TEXTURES / "raw"
TILE = 16
MAX_TILE_ID = 255
IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg"}
METADATA_NAME = "texture_scan.json"

@dataclass(frozen=True)
class ExistingTile:
    tile_id: int
    filename: str
    x: int
    y: int

@dataclass(frozen=True)
class Candidate:
    path: Path
    category: str
    cell: int
    image: Image.Image
    digest: str
    item_id: str | None = None
    block_id: str | None = None
    transparent: int | None = None

@dataclass
class ScanPlan:
    additions: list[Candidate]
    tile_ids: list[int]
    messages: list[str]
    item_rows: list[tuple[str, int]]
    block_rows: list[tuple[str, list[int], int]]

def csv_rows(path: Path) -> list[list[str]]:
    if not path.exists():
        return []
    with path.open(newline="") as stream:
        return list(csv.reader(stream))

def data_rows(path: Path, header: str) -> list[list[str]]:
    return [row for row in csv_rows(path)
            if row and not row[0].startswith("#") and row[0] != header]

def existing_tiles() -> tuple[list[ExistingTile], int]:
    result = []
    for row in data_rows(TEXTURES / "textures.csv", "tile_id"):
        if len(row) < 4:
            continue
        try:
            result.append(ExistingTile(int(row[0]), row[1], int(row[2]), int(row[3])))
        except ValueError:
            continue
    return result, max((tile.tile_id for tile in result), default=-1)

def load_image(path: Path) -> Image.Image | None:
    try:
        with Image.open(path) as image:
            return image.convert("RGBA")
    except Exception as exc:  # Pillow exception types vary by format.
        print(f"skip {path.relative_to(ROOT)}: cannot decode ({exc})")
        return None

def fit_icon(image: Image.Image) -> Image.Image:
    image = image.convert("RGBA")
    alpha = image.getchannel("A")
    if alpha.getbbox():
        image = image.crop(alpha.getbbox())
    side = min(image.size)
    if image.width != image.height:
        left, top = (image.width - side) // 2, (image.height - side) // 2
        image = image.crop((left, top, left + side, top + side))
    image.thumbnail((14, 14), Image.Resampling.LANCZOS)
    tile = Image.new("RGBA", (TILE, TILE), (0, 0, 0, 0))
    tile.alpha_composite(image, ((TILE - image.width) // 2, (TILE - image.height) // 2))
    return tile

def file_metadata(metadata: dict, relative: str) -> dict:
    for entry in metadata.get("assets", []):
        if isinstance(entry, dict) and entry.get("file") == relative:
            return entry
    return {}

def metadata_for(metadata: dict, relative: str, cell: int) -> dict:
    entry = file_metadata(metadata, relative)
    cells = entry.get("cells", {})
    value = cells.get(str(cell), cells.get(cell, entry)) if isinstance(cells, dict) else entry
    return value if isinstance(value, dict) else {}

def asset_cells(image: Image.Image, grid: bool) -> list[Image.Image]:
    if not grid and image.width == image.height and image.width >= TILE:
        return [fit_icon(image)]
    if grid and image.width % TILE == 0 and image.height % TILE == 0:
        return [image.crop((x, y, x + TILE, y + TILE))
                for y in range(0, image.height, TILE)
                for x in range(0, image.width, TILE)]
    raise ValueError(f"dimensions {image.width}x{image.height} require explicit grid metadata")

def pack_pixels(tiles: Iterable[ExistingTile]) -> set[bytes]:
    pixels: set[bytes] = set()
    cache: dict[Path, Image.Image] = {}
    for tile in tiles:
        path = TEXTURES / tile.filename
        image = cache.get(path)
        if image is None:
            image = load_image(path)
            if image is None:
                continue
            cache[path] = image
        box = (tile.x * TILE, tile.y * TILE, (tile.x + 1) * TILE, (tile.y + 1) * TILE)
        if box[2] <= image.width and box[3] <= image.height:
            pixels.add(image.crop(box).tobytes())
    return pixels

def registry_names() -> dict[str, list[str]]:
    result: dict[str, list[str]] = {}
    for row in csv_rows(ROOT / "data" / "registry" / "items.csv"):
        if len(row) >= 2 and row[0] and not row[0].startswith("#") and row[0] != "int":
            result.setdefault(row[1].lower(), []).append(row[0])
    return result

def read_metadata() -> dict:
    path = TEXTURES / METADATA_NAME
    if not path.exists():
        return {"assets": []}
    try:
        value = json.loads(path.read_text())
    except json.JSONDecodeError as exc:
        raise ValueError(f"invalid {path}: {exc}") from exc
    return value if isinstance(value, dict) else {"assets": []}

def scan() -> ScanPlan:
    tiles, max_tile = existing_tiles()
    known_pixels = pack_pixels(tiles)
    names = registry_names()
    metadata = read_metadata()
    candidates: list[Candidate] = []
    messages: list[str] = []
    seen_pixels = set(known_pixels)
    if not RAW.exists():
        return ScanPlan([], [], [f"raw directory missing: {RAW}"], [], [])
    files = [path for path in RAW.rglob("*")
             if path.is_file() and path.suffix.lower() in IMAGE_EXTENSIONS
             and not any(part.startswith(".") or part.endswith("~") for part in path.parts)]
    for path in sorted(files, key=lambda p: p.relative_to(RAW).as_posix()):
        image = load_image(path)
        if image is None:
            continue
        relative = path.relative_to(RAW).as_posix()
        try:
            cells = asset_cells(image, bool(file_metadata(metadata, relative).get("grid", False)))
        except ValueError as exc:
            messages.append(f"skip {path.relative_to(ROOT)}: {exc}")
            continue
        category = path.relative_to(RAW).parts[0]
        matches = names.get(path.stem.lower(), [])
        for cell_number, cell in enumerate(cells):
            digest = cell.tobytes()
            if digest in seen_pixels:
                continue
            seen_pixels.add(digest)
            meta = metadata_for(metadata, relative, cell_number)
            item_id = meta.get("item_id")
            if item_id is None and len(cells) == 1 and len(matches) == 1:
                item_id = matches[0]
            elif len(cells) == 1 and len(matches) > 1:
                messages.append(f"ambiguous registry match for {relative}: {matches}")
            candidates.append(Candidate(path, category, cell_number, cell,
                                        hashlib.sha256(digest).hexdigest(), item_id,
                                        meta.get("block_id"), meta.get("transparent")))
    next_id = max_tile + 1
    if candidates and next_id + len(candidates) - 1 > MAX_TILE_ID:
        messages.append("scan exceeds 256 logical texture IDs; no additions planned")
        candidates = []
    existing_items = {row[0] for row in data_rows(TEXTURES / "item_icons.csv", "item_id") if row}
    existing_blocks = {row[0] for row in data_rows(TEXTURES / "block_faces.csv", "block_id") if row}
    tile_ids: list[int] = []
    item_rows: list[tuple[str, int]] = []
    block_rows: list[tuple[str, list[int], int]] = []
    for candidate in candidates:
        tile_id = next_id
        next_id += 1
        tile_ids.append(tile_id)
        if candidate.item_id and candidate.item_id not in existing_items:
            item_rows.append((candidate.item_id, tile_id))
        if candidate.block_id and candidate.block_id not in existing_blocks:
            block_rows.append((candidate.block_id, [tile_id] * 6, int(candidate.transparent or 0)))
    return ScanPlan(candidates, tile_ids, messages, item_rows, block_rows)

def append_text(path: Path, lines: list[str]) -> None:
    if not lines:
        return
    original = path.read_text() if path.exists() else ""
    if original and not original.endswith("\n"):
        original += "\n"
    path.write_text(original + "\n".join(lines) + "\n")

def write_pack(path: Path, images: list[Image.Image]) -> tuple[int, int]:
    width = min(16, max(1, len(images))) * TILE
    height = ((len(images) + 15) // 16) * TILE
    sheet = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    for index, image in enumerate(images):
        sheet.alpha_composite(image, ((index % 16) * TILE, (index // 16) * TILE))
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name, suffix=".tmp", dir=path.parent)
    os.close(fd)
    try:
        sheet.save(temporary, format="PNG")
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return width // TILE, height // TILE

def write_manifest_entry(category: str) -> None:
    path = TEXTURES / "packs" / "manifest.json"
    if not path.exists():
        return
    try:
        manifest = json.loads(path.read_text())
    except json.JSONDecodeError:
        return
    packs = manifest.setdefault("packs", [])
    name = f"additive_{category}"
    if name not in packs:
        packs.append(name)
        path.write_text(json.dumps(manifest, indent=2) + "\n")

def apply(plan: ScanPlan) -> None:
    grouped: dict[str, list[tuple[Candidate, int]]] = {}
    for candidate, tile_id in zip(plan.additions, plan.tile_ids):
        grouped.setdefault(candidate.category, []).append((candidate, tile_id))
    for category, entries in sorted(grouped.items()):
        pack_name = f"additive_{category}.png"
        write_pack(TEXTURES / "packs" / pack_name, [candidate.image for candidate, _ in entries])
        width = min(16, max(1, len(entries)))
        append_text(TEXTURES / "textures.csv", [
            f"{tile_id},packs/{pack_name},{index % width},{index // width},0"
            for index, (_, tile_id) in enumerate(entries)])
        write_manifest_entry(category)
    append_text(TEXTURES / "item_icons.csv", [f"{item},{tile}" for item, tile in plan.item_rows])
    append_text(TEXTURES / "block_faces.csv", [
        f"{block},{','.join(map(str, faces))},{transparent}"
        for block, faces, transparent in plan.block_rows])

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true", help="append planned mappings; default is report-only")
    args = parser.parse_args()
    try:
        plan = scan()
    except ValueError as exc:
        parser.error(str(exc))
    for message in plan.messages:
        print(message)
    if not plan.additions:
        print("No new texture cells found.")
        return 0
    print(f"Planned {len(plan.additions)} new texture cells.")
    if args.apply:
        apply(plan)
        print("Applied additive texture mappings.")
    else:
        print("Dry run: pass --apply to write additions.")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
