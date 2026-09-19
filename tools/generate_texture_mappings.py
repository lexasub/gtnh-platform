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


def _find_textures_dir() -> Path:
    for candidate in (ROOT / "data" / "textures",
                      ROOT / "src" / "content" / "data" / "textures"):
        if candidate.is_dir():
            return candidate
    return ROOT / "data" / "textures"


TEXTURES = _find_textures_dir()
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
    faces: list[int] | None = None
    transparent: int | None = None
    merge_base: int | None = None
    merge_overlay: int | None = None

@dataclass
class ScanPlan:
    additions: list[Candidate]
    tile_ids: list[int]
    messages: list[str]
    item_rows: list[tuple[str, int]]
    block_rows: list[tuple[str, list[int], int]]
    merge_rows: list[tuple[int, int, int]]

def csv_rows(path: Path) -> list[list[str]]:
    if not path.exists():
        return []
    with path.open(newline="") as stream:
        return list(csv.reader(stream))

def data_rows(path: Path, header: str) -> list[list[str]]:
    def comment(row: list[str]) -> bool:
        return row[0].startswith("#") or row[0].lstrip("0123456789").startswith("#")
    return [row for row in csv_rows(path)
            if row and not comment(row) and row[0] != header]

@dataclass(frozen=True)
class RegistryState:
    tiles: list[ExistingTile]
    max_tile_id: int
    items: set[str]
    blocks: set[str]
    merges: dict[int, tuple[int, int]]  # composite_id -> (base_tile_id, overlay_tile_id)
    errors: list[str]


def read_registry() -> RegistryState:
    """Read and validate existing texture registries as immutable input."""
    tiles: list[ExistingTile] = []
    items: set[str] = set()
    blocks: set[str] = set()
    merges: dict[int, tuple[int, int]] = {}
    errors: list[str] = []
    seen_tiles: dict[int, str] = {}
    pending_item_refs: list[tuple[str, int]] = []
    pending_face_refs: list[tuple[str, list[int]]] = []
    for row in data_rows(TEXTURES / "textures.csv", "tile_id"):
        location = ",".join(row)
        if len(row) < 4:
            errors.append(f"malformed textures.csv row (need 4 columns): {location}")
            continue
        try:
            tile = ExistingTile(int(row[0]), row[1], int(row[2]), int(row[3]))
        except ValueError:
            errors.append(f"malformed textures.csv row (non-integer): {location}")
            continue
        if not 0 <= tile.tile_id <= MAX_TILE_ID:
            errors.append(f"tile_id {tile.tile_id} outside 0..{MAX_TILE_ID}")
        if tile.tile_id in seen_tiles:
            errors.append(f"duplicate tile_id {tile.tile_id} ({seen_tiles[tile.tile_id]} and {tile.filename})")
        seen_tiles[tile.tile_id] = tile.filename
        if tile.x < 0 or tile.y < 0:
            errors.append(f"negative cell position for tile_id {tile.tile_id}")
        tiles.append(tile)
    for row in data_rows(TEXTURES / "item_icons.csv", "item_id"):
        if len(row) < 2:
            errors.append(f"malformed item_icons.csv row: {','.join(row)}")
            continue
        try:
            tile_id = int(row[1])
        except ValueError:
            errors.append(f"malformed item_icons.csv row (non-integer tile): {','.join(row)}")
            continue
        pending_item_refs.append((row[0], tile_id))
        if row[0] in items:
            errors.append(f"duplicate item_id in item_icons.csv: {row[0]}")
        items.add(row[0])
    for row in data_rows(TEXTURES / "block_faces.csv", "block_id"):
        if len(row) < 8:
            errors.append(f"malformed block_faces.csv row (need 8 columns): {','.join(row)}")
            continue
        try:
            faces = [int(value) for value in row[1:7]]
            int(row[7])
        except ValueError:
            errors.append(f"malformed block_faces.csv row (non-integer): {','.join(row)}")
            continue
        if row[0] in blocks:
            errors.append(f"duplicate block_id in block_faces.csv: {row[0]}")
        blocks.add(row[0])
        pending_face_refs.append((row[0], faces))
    for row in data_rows(TEXTURES / "textures_merge.csv", "composite_id"):
        if len(row) < 3:
            errors.append(f"malformed textures_merge.csv row: {','.join(row)}")
            continue
        try:
            composite, base, overlay = int(row[0]), int(row[1]), int(row[2])
        except ValueError:
            errors.append(f"malformed textures_merge.csv row (non-integer): {','.join(row)}")
            continue
        if composite in merges:
            errors.append(f"duplicate composite_id in textures_merge.csv: {composite}")
        elif composite in seen_tiles and seen_tiles[composite] != "composite":
            if base == composite or overlay == composite:
                # Overlay-source pattern: the textures.csv row holds the raw
                # overlay pixels for this composite (added by apply()).
                pass
            else:
                errors.append(
                    f"composite_id {composite} collides with a textures.csv tile_id")
        for label, ref in (("base", base), ("overlay", overlay)):
            if ref not in seen_tiles and ref not in merges:
                errors.append(f"merge {composite} references unknown {label} tile {ref}")
        merges[composite] = (base, overlay)
        seen_tiles.setdefault(composite, "composite")  # merge tiles become valid refs
    for item_id, tile_id in pending_item_refs:
        if tile_id not in seen_tiles and tile_id not in merges:
            errors.append(f"item_icons.csv row references unknown tile {tile_id}: {item_id}")
    for block_id, faces in pending_face_refs:
        for face in faces:
            if face not in seen_tiles and face not in merges:
                errors.append(f"block_faces.csv row references unknown tile {face}: {block_id}")
    max_tile = max([tile.tile_id for tile in tiles] + list(merges), default=-1)
    return RegistryState(tiles, max_tile, items, blocks, merges, errors)

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
    bbox = alpha.getbbox()
    if bbox:
        cropped = image.crop(bbox)
        # A full-extent bbox means edge pixels are already non-transparent;
        # cropping to it is a no-op, so keep the original frame.
        if bbox != (0, 0, image.width, image.height):
            image = cropped
    side = min(image.size)
    if image.width != image.height:
        left, top = (image.width - side) // 2, (image.height - side) // 2
        image = image.crop((left, top, left + side, top + side))
    if image.width > 14:
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
        if image.width == TILE:
            return [image.copy()]  # exact single cell: keep pixels as-is
        return [fit_icon(image)]
    if image.width % TILE == 0 and image.height % TILE == 0:
        if not grid and (image.width != TILE or image.height != TILE):
            raise ValueError(
                f"dimensions {image.width}x{image.height} hold multiple cells; "
                "set \"grid\": true in texture_scan.json to import them")
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
        else:
            print(f"warning: tile {tile.tile_id} cell ({tile.x},{tile.y}) outside {tile.filename}")
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

def transparent_flag(value: object, label: str, messages: list[str]) -> int:
    if value in (None, 0, "0", False):
        return 0
    if value in (1, "1", True):
        return 1
    messages.append(f"invalid transparent value for {label}: {value!r}; using 0")
    return 0


def metadata_tile(value: object, label: str, messages: list[str]) -> int | None:
    if value is None:
        return None
    try:
        tile = int(value)
    except (TypeError, ValueError):
        messages.append(f"invalid tile reference for {label}: {value!r}; ignored")
        return None
    if not 0 <= tile <= MAX_TILE_ID:
        messages.append(f"tile reference out of range for {label}: {tile}; ignored")
        return None
    return tile


def scan() -> ScanPlan:
    """Read/validate/plan phase: build a purely additive plan or fail closed."""
    registry = read_registry()
    if registry.errors:
        raise ValueError("existing texture registries are invalid:\n  "
                         + "\n  ".join(registry.errors))
    known_pixels = pack_pixels(registry.tiles)
    names = registry_names()
    metadata = read_metadata()
    candidates: list[Candidate] = []
    messages: list[str] = []
    seen_pixels = set(known_pixels)
    if not RAW.exists():
        return ScanPlan([], [], [f"raw directory missing: {RAW}"], [], [], [])
    files = [path for path in RAW.rglob("*")
             if path.is_file() and path.suffix.lower() in IMAGE_EXTENSIONS
             and not any(part.startswith(".") or part.endswith("~") for part in path.parts)]
    for path in sorted(files, key=lambda p: p.relative_to(RAW).as_posix()):
        relative = path.relative_to(RAW).as_posix()
        entry = file_metadata(metadata, relative)
        image = load_image(path)
        if image is None:
            continue
        try:
            cells = asset_cells(image, bool(entry.get("grid", False)))
        except ValueError as exc:
            messages.append(f"skip {path.relative_to(ROOT)}: {exc}")
            continue
        category = path.relative_to(RAW).parts[0]
        matches = names.get(path.stem.lower(), [])
        seen_cells: set[bytes] = set()
        for cell_number, cell in enumerate(cells):
            digest = cell.tobytes()
            meta = metadata_for(metadata, relative, cell_number)
            label = f"{relative} cell {cell_number}"
            if digest in seen_pixels:
                continue
            if digest in seen_cells:
                messages.append(f"duplicate identical cells within {relative}; "
                                "only the first cell is planned")
                continue
            seen_cells.add(digest)
            item_id = meta.get("item_id")
            block_id = meta.get("block_id")
            transparent = transparent_flag(meta.get("transparent"), label, messages)
            merge_base = metadata_tile(meta.get("merge_base"), label, messages)
            merge_overlay = metadata_tile(meta.get("merge_overlay"), label, messages)
            if item_id is None and block_id is None and merge_base is None:
                if len(cells) == 1 and len(matches) == 1:
                    item_id = matches[0]
                elif len(cells) == 1 and len(matches) > 1:
                    messages.append(f"ambiguous registry match for {relative}: {matches}")
                else:
                    messages.append(f"unmapped {label}: no registry match and no metadata")
            candidates.append(Candidate(path, category, cell_number, cell,
                                        hashlib.sha256(digest).hexdigest(), item_id,
                                        block_id, None, transparent,
                                        merge_base, merge_overlay))
    next_id = registry.max_tile_id + 1
    if candidates and next_id + len(candidates) - 1 > MAX_TILE_ID:
        messages.append("scan exceeds 256 logical texture IDs; no additions planned")
        candidates = []
    return plan_rows(candidates, registry, next_id, names, messages)

def plan_rows(candidates: list[Candidate], registry: RegistryState, next_id: int,
              names: dict[str, list[str]], messages: list[str]) -> ScanPlan:
    """Assign append-only tile IDs and derive CSV rows with reference validation."""
    tile_ids: list[int] = []
    item_rows: list[tuple[str, int]] = []
    block_rows: list[tuple[str, list[int], int]] = []
    merge_rows: list[tuple[int, int, int]] = []
    planned_items = set(registry.items)
    planned_blocks = set(registry.blocks)
    known_tiles = {tile.tile_id for tile in registry.tiles} | set(registry.merges)
    for candidate in candidates:
        tile_id = next_id
        next_id += 1
        tile_ids.append(tile_id)
        known_tiles.add(tile_id)
        if candidate.merge_base is not None:
            # merge_overlay defaults to the candidate's own tile: the raw art
            # is the overlay layer, the composite tile reuses the same id.
            overlay = candidate.merge_overlay if candidate.merge_overlay is not None else tile_id
            if candidate.merge_base not in known_tiles or overlay not in known_tiles:
                messages.append(
                    f"merge for {candidate.path.stem} references unknown base/overlay tile "
                    f"({candidate.merge_base}, {overlay}); merge skipped")
            else:
                # merge_overlay may equal the candidate's own tile id: the raw
                # art is the overlay layer and the composite reuses the same id
                # (see apply(): such cells go to the merge pack, never loaded
                # as a standalone atlas slot).
                merge_rows.append((tile_id, candidate.merge_base, overlay))
        if candidate.item_id:
            if candidate.item_id not in planned_items:
                planned_items.add(candidate.item_id)
                item_rows.append((candidate.item_id, tile_id))
            else:
                messages.append(f"item {candidate.item_id} already has an icon row; kept existing")
        if candidate.block_id:
            if candidate.block_id not in planned_blocks:
                planned_blocks.add(candidate.block_id)
                block_rows.append((candidate.block_id, [tile_id] * 6,
                                   candidate.transparent or 0))
            else:
                messages.append(f"block {candidate.block_id} already has a face row; kept existing")
    return ScanPlan(candidates, tile_ids, messages, item_rows, block_rows, merge_rows)


def append_text(path: Path, lines: list[str]) -> None:
    if not lines:
        return
    original = path.read_text() if path.exists() else ""
    if original and not original.endswith("\n"):
        original += "\n"
    path.write_text(original + "\n".join(lines) + "\n")

def save_png_atomic(path: Path, sheet: Image.Image) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name, suffix=".tmp", dir=path.parent)
    os.close(fd)
    try:
        sheet.save(temporary, format="PNG")
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)

def pack_columns(count: int) -> int:
    """Deterministic sheet width: the smallest value that keeps the sheet
    row-major filled; appended cells only ever extend the sheet downwards."""
    return max(1, min(16, count))

def write_pack(path: Path, images: list[Image.Image], columns: int) -> None:
    columns = pack_columns(columns)
    rows = (len(images) + columns - 1) // columns
    sheet = Image.new("RGBA", (columns * TILE, rows * TILE), (0, 0, 0, 0))
    for index, image in enumerate(images):
        sheet.alpha_composite(image, ((index % columns) * TILE, (index // columns) * TILE))
    save_png_atomic(path, sheet)

def write_manifest_entry(category: str) -> None:
    path = TEXTURES / "packs" / "manifest.json"
    if not path.exists():
        return
    try:
        manifest = json.loads(path.read_text())
    except json.JSONDecodeError:
        return
    packs = manifest.setdefault("packs", [])
    names = (f"additive_{category}", "additive_merge")
    if any(name not in packs for name in names):
        packs.extend(name for name in names if name not in packs)
        path.write_text(json.dumps(manifest, indent=2) + "\n")

def apply(plan: ScanPlan) -> None:
    """Write phase: append planned pack cells and CSV rows; never touch existing bytes."""
    grouped: dict[str, list[tuple[Candidate, int]]] = {}
    for candidate, tile_id in zip(plan.additions, plan.tile_ids):
        grouped.setdefault(candidate.category, []).append((candidate, tile_id))
    for category, entries in sorted(grouped.items()):
        columns = pack_columns(len(entries))
        # Cells claimed by merge metadata are composited by the client at load
        # time from textures_merge.csv; their raw pixels go to the merge pack
        # and must never occupy a standalone atlas slot (duplicate id).
        merged = [(c, t) for c, t in entries if c.merge_base is not None]
        plain = [(c, t) for c, t in entries if c.merge_base is None]
        if merged:
            merge_pack = "additive_merge.png"
            merge_path = TEXTURES / "packs" / merge_pack
            if merge_path.exists():
                raise ValueError(f"additive pack already exists: {merge_path}; "
                                 "refusing to overwrite manual or previously applied pixels")
            write_pack(merge_path, [candidate.image for candidate, _ in merged], columns)
            append_text(TEXTURES / "textures.csv", [
                f"{tile_id},packs/{merge_pack},{index % columns},{index // columns},0"
                for index, (_, tile_id) in enumerate(merged)])
        if plain:
            pack_name = f"additive_{category}.png"
            pack_path = TEXTURES / "packs" / pack_name
            if pack_path.exists():
                raise ValueError(f"additive pack already exists: {pack_path}; "
                                 "refusing to overwrite manual or previously applied pixels")
            write_pack(pack_path, [candidate.image for candidate, _ in plain], columns)
            append_text(TEXTURES / "textures.csv", [
                f"{tile_id},packs/{pack_name},{index % columns},{index // columns},0"
                for index, (_, tile_id) in enumerate(plain)])
        write_manifest_entry(category)
    append_text(TEXTURES / "item_icons.csv", [f"{item},{tile}" for item, tile in plan.item_rows])
    append_text(TEXTURES / "block_faces.csv", [
        f"{block},{','.join(map(str, faces))},{transparent}"
        for block, faces, transparent in plan.block_rows])
    append_text(TEXTURES / "textures_merge.csv", [
        f"{composite},{base},{overlay}"
        for composite, base, overlay in plan.merge_rows])

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
    for item, tile in plan.item_rows:
        print(f"  item icon: {item} -> tile {tile}")
    for block, faces, transparent in plan.block_rows:
        print(f"  block faces: {block} -> {faces} transparent={transparent}")
    for composite, base, overlay in plan.merge_rows:
        print(f"  merge: {composite} = {base} + {overlay}")
    if args.apply:
        apply(plan)
        print("Applied additive texture mappings.")
    else:
        print("Dry run: pass --apply to write additions.")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
