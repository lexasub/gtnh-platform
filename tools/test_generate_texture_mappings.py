"""Sandboxed tests for the additive texture scanner (tools/generate_texture_mappings.py)."""
from __future__ import annotations

import json
import shutil
import tempfile
import unittest
import unittest.mock
from pathlib import Path

from PIL import Image

from tools import generate_texture_mappings as gtm

REPO_ROOT = Path(__file__).resolve().parent.parent
_candidates = [REPO_ROOT / "data" / "textures",
               REPO_ROOT / "src" / "content" / "data" / "textures"]
REAL_TEXTURES = next((c for c in _candidates if c.is_dir()), _candidates[0])
TILE = gtm.TILE


def cell_image(color: tuple[int, int, int, int]) -> Image.Image:
    return Image.new("RGBA", (TILE, TILE), color)


def save_cell(path: Path, color: tuple[int, int, int, int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    cell_image(color).save(path)


class ScannerSandbox(unittest.TestCase):
    """Copies real registries/packs into a temp dir and points the scanner at it."""

    def setUp(self) -> None:
        self._tmp = Path(tempfile.mkdtemp(prefix="texture_scan_test_"))
        self.addCleanup(shutil.rmtree, self._tmp, True)
        self.textures = self._tmp / "textures"
        shutil.copytree(REAL_TEXTURES / "packs", self.textures / "packs")
        for name in ("textures.csv", "item_icons.csv", "block_faces.csv",
                     "textures_merge.csv"):
            shutil.copy(REAL_TEXTURES / name, self.textures / name)
        (self.textures / "raw").mkdir()
        items = self._tmp / "data" / "registry" / "items.csv"
        items.parent.mkdir(parents=True)
        items.write_text(
            "int,name,stack,meta\n"
            "0:0:42,alpha_block,,0\n"
            "0:110:1,iron_ingot,,0\n"
            "0:0:7,bricks,,0\n"
        )
        self._patchers = []
        for attr, value in (
            ("ROOT", self._tmp),
            ("TEXTURES", self.textures),
            ("RAW", self.textures / "raw"),
        ):
            patcher = unittest.mock.patch.object(gtm, attr, value)
            patcher.start()
            self._patchers.append(patcher)
        self.addCleanup(self._unpatch)

    def _unpatch(self) -> None:
        for patcher in self._patchers:
            patcher.stop()

    def registry_snapshot(self) -> dict[str, bytes]:
        snapshot = {}
        for name in ("textures.csv", "item_icons.csv", "block_faces.csv",
                     "textures_merge.csv"):
            snapshot[name] = (self.textures / name).read_bytes()
        for pack in sorted((self.textures / "packs").glob("*.png")):
            snapshot[f"packs/{pack.name}"] = pack.read_bytes()
        return snapshot

    def max_tile_id(self) -> int:
        return gtm.read_registry().max_tile_id

    def make_metadata(self, assets: list[dict]) -> None:
        (self.textures / gtm.METADATA_NAME).write_text(json.dumps({"assets": assets}))

    def add_raw(self, relative: str, color=(200, 10, 10, 255)) -> Path:
        path = self.textures / "raw" / relative
        save_cell(path, color)
        return path


# 2.1 no-op and idempotent scans -------------------------------------------------

class NoOpScanTests(ScannerSandbox):
    def test_empty_raw_dir_proposes_nothing(self):
        plan = gtm.scan()
        self.assertEqual(plan.additions, [])
        self.assertEqual(plan.item_rows, [])
        self.assertEqual(plan.block_rows, [])
        self.assertEqual(plan.merge_rows, [])

    def test_dry_run_modifies_nothing(self):
        self.add_raw("materials/alpha_block.png")
        before = self.registry_snapshot()
        plan = gtm.scan()
        self.assertEqual(len(plan.additions), 1)
        self.assertEqual(self.registry_snapshot(), before)

    def test_apply_then_rescan_is_idempotent(self):
        self.add_raw("materials/alpha_block.png")
        gtm.apply(gtm.scan())
        after_apply = self.registry_snapshot()
        self.assertIn("packs/additive_materials.png", after_apply)
        plan = gtm.scan()
        self.assertEqual(plan.additions, [])
        self.assertEqual(plan.item_rows, [])
        self.assertEqual(plan.merge_rows, [])
        self.assertEqual(self.registry_snapshot(), after_apply)

    def test_exact_pixel_duplicate_of_existing_pack_is_skipped(self):
        # tile 0 is terrain.png cell (0,0); recreate those pixels as raw art
        source = Image.open(self.textures / "packs" / "terrain.png").convert("RGBA")
        cell = source.crop((0, 0, TILE, TILE))
        path = self.textures / "raw" / "terrain" / "alpha_block.png"
        path.parent.mkdir(parents=True)
        cell.save(path)
        plan = gtm.scan()
        self.assertEqual(plan.additions, [])


# 2.2 preservation of manual rows and pack pixels --------------------------------

class PreservationTests(ScannerSandbox):
    def test_manual_rows_and_pixels_preserved_byte_for_byte(self):
        manual_item = "0:1110:999,23\n"  # fresh id; existing ids stay unique
        items_path = self.textures / "item_icons.csv"
        items_path.write_text(items_path.read_text() + manual_item)
        before = self.registry_snapshot()
        self.add_raw("casings/alpha_block.png")
        gtm.apply(gtm.scan())
        after = self.registry_snapshot()
        for name in before:
            if name in ("textures.csv", "item_icons.csv"):
                continue  # append-only targets for a name-matched asset
            self.assertEqual(after[name], before[name], f"{name} changed")
        # append-only: original bytes are a strict prefix of the new content
        self.assertTrue(after["textures.csv"].startswith(before["textures.csv"]))
        self.assertTrue(after["item_icons.csv"].startswith(before["item_icons.csv"]))
        self.assertTrue(items_path.read_text().endswith(manual_item + "0:0:42,47\n"))

    def test_comment_and_order_preserved(self):
        faces = self.textures / "block_faces.csv"
        original = faces.read_bytes()
        self.add_raw("casings/alpha_block.png")
        gtm.apply(gtm.scan())
        self.assertTrue(faces.read_bytes().startswith(original))

    def test_existing_item_and_block_rows_win(self):
        self.make_metadata([{"file": "materials/alpha_block.png",
                             "item_id": "0:110:1", "block_id": "0:0:1"}])
        self.add_raw("materials/alpha_block.png")
        plan = gtm.scan()
        # 0:110:1 and 0:0:1 already have rows -> tile planned but no new rows
        self.assertEqual(plan.item_rows, [])
        self.assertEqual(plan.block_rows, [])
        self.assertTrue(any("kept existing" in m for m in plan.messages))

    def test_existing_pack_never_overwritten(self):
        self.add_raw("terrain/alpha_block.png")
        gtm.apply(gtm.scan())
        before = self.registry_snapshot()
        self.add_raw("terrain/iron_ingot.png", (1, 2, 3, 255))
        with self.assertRaises(ValueError):
            gtm.apply(gtm.scan())
        self.assertEqual(self.registry_snapshot(), before)


# 2.3 exact / ambiguous / missing / malformed / duplicate / overflow -------------

class MatchingTests(ScannerSandbox):
    def test_exact_single_name_match_maps_item(self):
        # 'bricks' has a single registry entry with no existing icon row
        self.add_raw("materials/bricks.png")
        plan = gtm.scan()
        self.assertEqual(plan.item_rows, [("0:0:7", self.max_tile_id() + 1)])

    def test_name_match_with_existing_row_keeps_existing(self):
        # 'iron_ingot' already owns an item_icons.csv row -> no new row
        self.add_raw("materials/iron_ingot.png")
        plan = gtm.scan()
        self.assertEqual(plan.item_rows, [])
        self.assertTrue(any("kept existing" in m for m in plan.messages))

    def test_ambiguous_name_match_is_reported_not_mapped(self):
        (self._tmp / "data" / "registry" / "items.csv").write_text(
            "int,name,stack,meta\n0:0:1,dup,,0\n0:0:2,dup,,0\n")
        self.add_raw("materials/dup.png")
        plan = gtm.scan()
        self.assertEqual(plan.item_rows, [])
        self.assertEqual(plan.block_rows, [])
        self.assertTrue(any("ambiguous registry match" in m for m in plan.messages))

    def test_missing_match_is_reported_not_mapped(self):
        self.add_raw("materials/unknown_thing.png")
        plan = gtm.scan()
        self.assertEqual(plan.item_rows, [])
        self.assertTrue(any("unmapped" in m for m in plan.messages))

    def test_malformed_metadata_fails_closed(self):
        (self.textures / gtm.METADATA_NAME).write_text("{not json")
        with self.assertRaises(ValueError):
            gtm.scan()

    def test_malformed_registry_row_fails_closed(self):
        textures = self.textures / "textures.csv"
        textures.write_text(textures.read_text() + "broken,row\n")
        with self.assertRaises(ValueError) as ctx:
            gtm.scan()
        self.assertIn("malformed textures.csv row", str(ctx.exception))

    def test_duplicate_tile_id_fails_closed(self):
        textures = self.textures / "textures.csv"
        textures.write_text(textures.read_text() + "0,packs/terrain.png,0,0,0\n")
        with self.assertRaises(ValueError) as ctx:
            gtm.scan()
        self.assertIn("duplicate tile_id", str(ctx.exception))

    def test_duplicate_identical_cells_within_one_file(self):
        sheet = Image.new("RGBA", (2 * TILE, TILE))
        sheet.paste(cell_image((9, 9, 9, 255)), (0, 0))
        sheet.paste(cell_image((9, 9, 9, 255)), (TILE, 0))
        path = self.textures / "raw" / "materials" / "dupes.png"
        path.parent.mkdir(parents=True)
        sheet.save(path)
        self.make_metadata([{"file": "materials/dupes.png", "grid": True}])
        plan = gtm.scan()
        self.assertEqual(len(plan.additions), 1)
        self.assertTrue(any("duplicate identical cells" in m for m in plan.messages))

    def test_multicell_without_grid_metadata_is_skipped(self):
        sheet = Image.new("RGBA", (2 * TILE, TILE), (7, 7, 7, 255))
        path = self.textures / "raw" / "materials" / "sheet.png"
        path.parent.mkdir(parents=True)
        sheet.save(path)
        plan = gtm.scan()
        self.assertEqual(plan.additions, [])
        self.assertTrue(any("grid" in m for m in plan.messages))

    def test_overflow_blocks_all_additions(self):
        textures = self.textures / "textures.csv"
        # raise max tile id to 255 -> no capacity left
        textures.write_text(textures.read_text() + "255,packs/terrain.png,7,0,0\n")
        self.add_raw("materials/alpha_block.png")
        plan = gtm.scan()
        self.assertEqual(plan.additions, [])
        self.assertTrue(any("exceeds 256" in m for m in plan.messages))

    def test_tile_ids_are_append_only_above_existing_max(self):
        self.add_raw("materials/alpha_block.png")
        self.add_raw("casings/iron_ingot.png", (3, 4, 5, 255))
        plan = gtm.scan()
        base = self.max_tile_id()
        self.assertEqual(plan.tile_ids, [base + 1, base + 2])


# 2.4 texture/face/item/merge reference validation --------------------------------

class ReferenceValidationTests(ScannerSandbox):
    def test_item_icon_row_referencing_unknown_tile_fails(self):
        icons = self.textures / "item_icons.csv"
        icons.write_text(icons.read_text() + "0:110:1,222\n")
        with self.assertRaises(ValueError) as ctx:
            gtm.scan()
        self.assertIn("unknown tile 222", str(ctx.exception))

    def test_block_face_row_referencing_unknown_tile_fails(self):
        faces = self.textures / "block_faces.csv"
        faces.write_text(faces.read_text() + "0:0:42,222,222,222,222,222,222,0\n")
        with self.assertRaises(ValueError) as ctx:
            gtm.scan()
        self.assertIn("unknown tile 222", str(ctx.exception))

    def test_item_and_face_rows_may_reference_merge_composites(self):
        icons = self.textures / "item_icons.csv"
        icons.write_text(icons.read_text() + "0:0:42,20\n")  # 20 is an existing composite
        faces = self.textures / "block_faces.csv"
        faces.write_text(faces.read_text() + "0:0:42,21,21,21,21,21,21,0\n")
        gtm.scan()  # must not raise

    def test_merge_row_referencing_unknown_base_fails(self):
        merges = self.textures / "textures_merge.csv"
        merges.write_text(merges.read_text() + "60,222,37\n")
        with self.assertRaises(ValueError) as ctx:
            gtm.scan()
        self.assertIn("unknown base tile 222", str(ctx.exception))

    def test_merge_composite_colliding_with_tile_fails(self):
        merges = self.textures / "textures_merge.csv"
        merges.write_text(merges.read_text() + "0,40,37\n")
        with self.assertRaises(ValueError) as ctx:
            gtm.scan()
        self.assertIn("collides", str(ctx.exception))

    def test_new_merge_from_metadata_is_additive(self):
        self.make_metadata([{"file": "ores/alpha_block.png", "merge_base": 40}])
        self.add_raw("ores/alpha_block.png")
        plan = gtm.scan()
        composite = self.max_tile_id() + 1
        self.assertEqual(plan.merge_rows, [(composite, 40, composite)])
        gtm.apply(plan)
        text = (self.textures / "textures_merge.csv").read_text()
        self.assertIn(f"{composite},40,{composite}", text)
        # rescan: raw art pixels now exist in the additive pack -> nothing new
        self.assertEqual(gtm.scan().additions, [])
        # and the merge row is registered exactly once
        rows = gtm.data_rows(self.textures / "textures_merge.csv", "composite_id")
        self.assertEqual(len([r for r in rows if r[0] == str(composite)]), 1)

    def test_new_merge_with_unknown_base_is_skipped_not_applied(self):
        self.make_metadata([{"file": "ores/alpha_block.png", "merge_base": 222}])
        self.add_raw("ores/alpha_block.png")
        plan = gtm.scan()
        self.assertEqual(plan.merge_rows, [])
        self.assertTrue(any("merge skipped" in m for m in plan.messages))

    def test_block_id_metadata_creates_face_row(self):
        self.make_metadata([{"file": "casings/alpha_block.png",
                             "block_id": "0:0:42", "transparent": 1}])
        self.add_raw("casings/alpha_block.png")
        plan = gtm.scan()
        tile = self.max_tile_id() + 1
        self.assertEqual(plan.block_rows, [("0:0:42", [tile] * 6, 1)])
        gtm.apply(plan)
        self.assertIn(f"0:0:42,{','.join([str(tile)] * 6)},1",
                      (self.textures / "block_faces.csv").read_text())


if __name__ == "__main__":
    unittest.main()

