## Context

TextureAtlas consumes append-ordered CSV registries and a fixed 256-slot atlas. Existing CSV rows and pack pixels are canonical manual data.

## Decisions

- The scanner reads existing registries as immutable input and preserves their raw lines.
- Raw assets are discovered recursively under `data/textures/raw/<category>/`, in sorted path/cell order.
- Existing packs are never repacked or rewritten. New cells go to deterministic `packs/additive_<category>.png` files.
- New logical tile IDs start above the existing maximum and must remain within 0..255.
- Filename-to-registry matching requires exactly one item/block name match. Generic, missing, multi-match, and multi-cell ambiguous assets are reported but not mapped.
- Existing item, block-face, and merge rows always win. New composites require explicit metadata; filenames alone do not create merge recipes.
- Default mode is dry-run/report. `--apply` is required for writes.

## Safety

The scanner plans all changes before writing. It validates image decoding, 16-pixel cells, source paths, tile ranges, merge references, and atlas capacity. It writes planned CSV/PNG files atomically and fails closed on conflicts.
