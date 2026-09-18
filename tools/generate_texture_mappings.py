#!/usr/bin/env python3
"""Build canonical RGBA source packs and texture CSVs."""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Iterable

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
TEXTURES = ROOT / "data" / "textures"
TILE = 16

# Logical IDs are kept stable; only their source pack and cell change.
TERRAIN = [0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 40, 41, 42, 43, 44]
MATERIALS = [23, 24, 25]
MACHINES = list(range(26, 37))
ORES = [37, 38, 39]


def dedupe_images(images: list[tuple[str, Image.Image]]) -> list[tuple[str, Image.Image]]:
    """Keep one physical cell per identical RGBA image."""
    seen: dict[bytes, str] = {}
    unique: list[tuple[str, Image.Image]] = []
    for label, image in images:
        key = image.convert("RGBA").tobytes()
        if key not in seen:
            seen[key] = label
            unique.append((label, image))
    return unique


def position_for(images: list[tuple[str, Image.Image]], label: str) -> str:
    target = dict(images)[label].convert("RGBA").tobytes()
    for existing, image in images:
        if image.convert("RGBA").tobytes() == target:
            return existing
    raise KeyError(label) # pragma: no cover


def stable_cell_labels(ids: list[int], images: list[tuple[str, Image.Image]]) -> dict[int, str]:
    labels = [label for label, _ in images]
    return {tile_id: labels[index] for index, tile_id in enumerate(ids)}


def build_dedup_pack(name: str, images: list[tuple[str, Image.Image]], output: Path) -> tuple[dict[str, tuple[int, int]], dict[str, str]]:
    unique = dedupe_images(images)
    positions = pack(name, unique, output)
    aliases = {label: position_for(unique, label) for label, _ in images}
    return positions, aliases


def raw_category(name: str) -> str:
    return str(source(name).parent.name) # explicit raw category or textures root



def source(name: str) -> Path:
    candidates = [TEXTURES / name, TEXTURES / "source-originals" / name]
    candidates.extend((TEXTURES / "raw").glob(f"*/{name}"))
    for path in candidates:
        if path.exists():
            return path
    raise FileNotFoundError(name)


def fit_icon(name: str) -> Image.Image:
    image = Image.open(source(name)).convert("RGBA")
    alpha = image.getchannel("A")
    if alpha.getextrema() != (255, 255) and alpha.getbbox():
        image = image.crop(alpha.getbbox())
    side = min(image.size)
    if image.size[0] != image.size[1]:
        left, top = (image.width - side) // 2, (image.height - side) // 2
        image = image.crop((left, top, left + side, top + side))
    image.thumbnail((14, 14), Image.Resampling.LANCZOS)
    tile = Image.new("RGBA", (TILE, TILE), (0, 0, 0, 0))
    tile.alpha_composite(image, ((TILE - image.width) // 2, (TILE - image.height) // 2))
    return tile


def cell(name: str, x: int, y: int, rotate: int = 0) -> Image.Image:
    image = Image.open(source(name)).convert("RGBA")
    tile = image.crop((x * TILE, y * TILE, (x + 1) * TILE, (y + 1) * TILE))
    if rotate == 1:
        tile = tile.transpose(Image.Transpose.ROTATE_270)
    elif rotate == 2:
        tile = tile.transpose(Image.Transpose.ROTATE_180)
    elif rotate == 3:
        tile = tile.transpose(Image.Transpose.ROTATE_90)
    return tile


def pack(name: str, images: list[tuple[str, Image.Image]], output: Path) -> dict[str, tuple[int, int]]:
    output.mkdir(parents=True, exist_ok=True)
    width = 16 * TILE
    height = max(TILE, ((len(images) + 15) // 16) * TILE)
    sheet = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    positions: dict[str, tuple[int, int]] = {}
    for i, (label, image) in enumerate(images):
        x, y = i % 16, i // 16
        sheet.alpha_composite(image.convert("RGBA"), (x * TILE, y * TILE))
        positions[label] = (x, y)
    sheet.save(output / f"{name}.png")
    return positions


def terrain_images() -> list[tuple[str, Image.Image]]:
    # The old often palette was synthetic. Replace it with named terrain art.
    images = [
        ("stone", cell("stone.png", 1, 1)),
        ("cobblestone", cell("cobblestone.png", 1, 1)),
        ("dirt", cell("dirt.png", 0, 0)),
        ("grass", cell("dirt.png", 1, 1)),
        ("sand", fit_icon("sand.png")),
        ("bricks", fit_icon("bricks.png")),
        ("obsidian", cell("obsidian.png", 0, 0)),
        ("stone_bricks", fit_icon("bricks.png")),
        ("sandstone", fit_icon("sand.png")),
        ("legacy_gravel", cell("cobblestone.png", 1, 1)),
        ("legacy_wood", fit_icon("bricks.png")),
        ("legacy_leaves", cell("dirt.png", 1, 1)),
        ("legacy_glass", cell("dirt.png", 1, 1)),
        ("legacy_decorative", fit_icon("bricks.png")),
        ("dirt_rotated", cell("dirt.png", 0, 1, 1)),
        ("grass_dirt", cell("dirt.png", 1, 1)),
        ("dirt_named", cell("dirt.png", 0, 0)),
        ("grass_named", cell("dirt.png", 1, 1)),
        ("stone_named", cell("stone.png", 1, 1)),
        ("cobblestone_named", cell("cobblestone.png", 1, 1)),
        ("sand_named", fit_icon("sand.png")),
        ("bricks_named", fit_icon("bricks.png")),
        ("obsidian_named", cell("obsidian.png", 0, 0)),
    ]
    return images


def machine_images() -> list[tuple[str, Image.Image]]:
    files = [
        ("battery", "battary.png"), ("furnace", "furnace.png"),
        ("macerator", "macerator.png"), ("compressor", "compressor.png"),
        ("extractor", "extractor.png"), ("mixer", "mixer.png"),
        ("generator", "generator.png"), ("casing_lv", "machine_casing0.png"),
        ("casing_mv", "machine_casing1.png"), ("casing_hv", "machine_casing2.png"),
        ("basic_casing", "basic_casing.png"), ("boiler", "boiler.png"),
    ]
    return [(label, fit_icon(filename)) for label, filename in files]


def read_items() -> dict[str, str]:
    with (ROOT / "data" / "registry" / "items.csv").open(newline="") as stream:
        return {row[0]: row[1] for row in csv.reader(stream)
                if row and not row[0].startswith("#") and row[0] != "int"}


def item_tile(name: str, old: int | None) -> int | None:
    exact = {
        "heat_furnace": 27, "electric_furnace_lv": 27, "electric_furnace_mv": 27,
        "electric_furnace_hv": 27, "electric_blast_furnace": 27, "hot_blast_furnace": 27,
        "heat_macerator": 28, "steam_macerator": 28, "rotare_macerator": 28,
        "macerator_lv": 28, "macerator_mv": 28, "steam_compressor": 29,
        "compressor_lv": 29, "compressor_mv": 29, "implosion_compressor": 29,
        "steam_extractor": 30, "extractor_lv": 30, "extractor_mv": 30,
        "steam_mixer": 31, "mixer_lv": 31, "mixer_mv": 31,
        "heat_generator": 32, "creative_generator": 32, "rotare_generator": 32,
    }
    if name in exact:
        return exact[name]
    if name in {"gold_ore", "redstone_ore"}:
        return 20 if name == "gold_ore" else 21
    if name == "uranium_ore":
        return 39
    if name.endswith(("_ingot", "_plate", "_dust")):
        return 23
    if name.endswith("_rod") or name == "rod":
        return 24
    if name.endswith(("_gear", "_ring", "_rotor", "_spring")):
        return 25
    if name.endswith("_ore"):
        return 40
    if name.startswith("battery_"):
        return 26
    if name.startswith(("hull_", "transformer_")):
        return 33 if "lv" in name else 34 if "mv" in name else 35
    if name in {"alloy_smelter_lv", "bender", "centrifuge_lv", "chemical_reactor_lv", "electrolyzer_lv", "extruder", "wiremill", "forge_hammer_lv", "ore_washer_lv", "thermal_centrifuge_lv", "assembling_machine_lv", "macerator_mv", "compressor_mv", "extractor_mv", "mixer_mv", "alloy_smelter_mv", "assembling_machine_mv", "assembling_machine_hv", "circuit_assembler", "precision_laser_welder", "vacuum_freezer", "industrial_smelter", "industrial_grinder", "industrial_squeezer", "industrial_brewery", "large_turbine", "multi_smelter", "cracker", "distillation_tower", "chemical_plant", "pyrolyse_oven", "large_combustion_reactor", "assembly_line", "extreme_combustion_reactor", "naquadah_reactor", "coke_oven", "steam_turbine", "large_chemical_reactor", "advanced_database"}:
        return 33 if "lv" in name else 34 if "mv" in name else 35 if "hv" in name else 36
    return 25 if old == 5 else old


def rewrite_csvs(output: Path) -> None:
    items = read_items()
    existing_icons = TEXTURES / "item_icons.csv"
    icon_rows = ["item_id,tile_id", "# Explicit item icon overrides; block items fall back to block_faces."]
    seen: set[str] = set()
    with existing_icons.open(newline="") as stream:
        for row in csv.reader(stream):
            if len(row) < 2 or row[0].startswith("#") or row[0] == "item_id":
                continue
            try:
                tile = item_tile(items.get(row[0], ""), int(row[1]))
            except ValueError:
                continue
            if tile is not None:
                icon_rows.append(f"{row[0]},{tile}")
                seen.add(row[0])
    additions = [(item_id, item_tile(name, None)) for item_id, name in items.items() if item_id not in seen]
    additions = [(item_id, tile) for item_id, tile in additions if tile is not None]
    icon_rows += ["", "# GENERATED named/family mappings"]
    icon_rows += [f"{item_id},{tile}" for item_id, tile in additions]
    (output / "item_icons.csv").write_text("\n".join(icon_rows) + "\n")

    old_faces = TEXTURES / "block_faces.csv"
    face_rows = ["block_id,face_px,face_nx,face_py,face_ny,face_pz,face_nz,transparent"]
    terrain = {"0:0:1": [40] * 6, "0:0:2": [41] * 6, "0:0:3": [42] * 6, "0:0:5": [44] * 6, "0:0:6": [43] * 6, "0:0:7": [18] * 6, "0:0:8": [17, 17, 17, 18, 17, 17], "0:0:9": [43] * 6, "0:0:17": [43] * 6}
    with old_faces.open(newline="") as stream:
        for row in csv.reader(stream):
            if len(row) != 8 or row[0].startswith("#") or row[0] == "block_id":
                continue
            if row[0] in terrain:
                row[1:7] = [str(v) for v in terrain[row[0]]]
            elif items.get(row[0], "").endswith("_ore"):
                name = items[row[0]]
                tile = 20 if name == "gold_ore" else 21 if name == "redstone_ore" else 39 if name == "uranium_ore" else 40
                row[1:7] = [str(tile)] * 6
            if row[0] == "0:0:4":
                row[7] = "1"
            face_rows.append(",".join(row))
    (output / "block_faces.csv").write_text("\n".join(face_rows) + "\n")


def build(output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    terrain = terrain_images()
    material = [("ingot", fit_icon("ingot.png")), ("rod", fit_icon("rod.png")), ("gear", fit_icon("gear.png"))]
    machines = machine_images()
    ores = [("gold_overlay", fit_icon("gold_ore_overlay.png")), ("redstone_overlay", fit_icon("redstone_ore_overlay.png")), ("uranium", fit_icon("uranium_ore_overlay.png"))]
    tp = pack("terrain", terrain, output)
    mp = pack("materials", material, output)
    xp = pack("machines", machines, output)
    op = pack("ores", ores, output) # only tiles 37-39 are source art; 20/21 are virtual composites
    # Keep the old generated tile IDs used by the current item mappings, but
    # source them from the category packs rather than generated_icons.png.
    # IDs 45 and 46 provide explicit boiler/casing tiles for future mappings.
    extra = [("boiler_extra", fit_icon("boiler.png")), ("lv_casing_extra", fit_icon("machine_casing0.png"))]
    ep = pack("machine_extras", extra, output)
    positions: dict[int, tuple[str, tuple[int, int]]] = {}
    for tile_id, label in zip(TERRAIN, [x[0] for x in terrain]):
        positions[tile_id] = ("packs/terrain.png", tp[label])
    for tile_id, (label, _) in zip(MATERIALS, material):
        positions[tile_id] = ("packs/materials.png", mp[label])
    for tile_id, (label, _) in zip(MACHINES, machines[:len(MACHINES)]):
        positions[tile_id] = ("packs/machines.png", xp[label])
    for tile_id, (label, _) in zip(ORES, ores):
        positions[tile_id] = ("packs/ores.png", op[label])
    positions[45] = ("packs/machine_extras.png", ep["boiler_extra"])
    positions[46] = ("packs/machine_extras.png", ep["lv_casing_extra"])
    rows = ["tile_id,filename,tile_x,tile_y,rotate"]
    # Extras are deliberately not assigned to item IDs yet.
    for tile_id in EXTRA_MACHINES:
        filename, (x, y) = positions[tile_id]
        rows.append(f"{tile_id},{filename},{x},{y},0")
    for tile_id in TERRAIN + MATERIALS + MACHINES + ORES:
        filename, (x, y) = positions[tile_id]
        rows.append(f"{tile_id},{filename},{x},{y},0")
    (output / "textures.csv").write_text("\n".join(rows) + "\n")
    rewrite_csvs(output)
    (output / "textures_merge.csv").write_text("composite_id,base_tile_id,overlay_tile_id\n20,40,37\n21,40,38\n")
    (output / "manifest.json").write_text(json.dumps({"tile_size": TILE, "packs": ["terrain", "materials", "machines", "ores"], "source_policy": "RGBA; exact terrain cells and fit_icon for large art"}, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / ".texture-pack-output")
    build(parser.parse_args().output)
