## Context

The runtime maps logical tile IDs to physical atlas slots during `TextureAtlas::Init`. Today the physical grid is 16×16 and the slot counter is 8-bit. Tile metadata, source positions, and rotations are stored in 256-entry arrays. The authoring scanner independently caps IDs at 255. Current texture data has 251 `textures.csv` source rows and 24 merge recipes; 21 recipes are self-merges (`composite_id == overlay_tile_id`). Loading each self-merge source as a standalone slot and then allocating its composite requires 275 physical slots, so the current loader wraps/exceeds its grid.

## Goals / Non-Goals

- Goals: support 1,024 logical tile IDs and 1,024 physical slots; retain 16×16 tile art and existing UV semantics; reject invalid references safely; keep scanner, runtime, tests, docs, and spec bounds synchronized.
- Non-Goals: change tile pixel size, migrate to a texture array, change source-pack coordinates, remap existing content, or redesign GPU rendering.

## Decisions

- Use a 32×32 tile grid in a 512×512 RGBA8 texture. This is the documented atlas growth path and keeps the current single-texture sampling model.
- Define the valid logical tile range as 0–1023. Existing public APIs use `uint16_t`, which already represents this range.
- Widen per-tile tables and the physical slot counter to at least 16 bits; calculate slot coordinates in a wide type and check capacity before writing.
- For only self-merge rows (`composite_id == overlay_tile_id`), preserve source filename/cell/rotation metadata but do not allocate/load the overlay as a standalone atlas slot. The merge pass reads the overlay from its source PNG and allocates the final composite once. Ordinary overlays with distinct composite and overlay IDs retain their standalone slots.
- Keep a runtime capacity guard even though the scanner enforces the same bound; malformed/stale registries must not index arrays or write outside the atlas.

## Risks / Trade-offs

- Atlas GPU memory increases fourfold, from 256 KiB to 1 MiB; this remains small for the client.
- Runtime, scanner, and specs can silently diverge if any bound remains 255/256. Boundary tests and a search for capacity assumptions are part of the change.
- Incorrect self-merge filtering could lose overlay source metadata. The implementation must keep source coordinates while suppressing only standalone slot allocation, and verify both self-merge and ordinary merge cases.

## Migration Plan

No content migration is required: existing tile IDs remain unchanged. Update runtime and scanner capacity, then verify all current CSV references and composites against the larger capacity. Keep unrelated worktree changes intact.

## Open Questions

None. The repository already documents 512×512 / 1,024 tiles as the next atlas size.
