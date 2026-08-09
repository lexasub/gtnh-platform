# Change: Add Oak Tree Generation (survival oak planks)

## Why

The world has no trees, so `oak_planks` is unobtainable through gameplay: the registry contains oak planks (used by sticks, pickaxes, crafting tables, quests) but nothing that produces them. Trees must generate naturally in the world and drop logs when mined; logs must craft into planks.

## What Changes

- **New `SurfaceHeights` module** (`src/services/world_generator/SurfaceHeights.h/.cpp`) — sole owner of the height formula (`64 + baseFBM*12 + contFBM*20`); FBM nodes move out of the file-local `anonymous namespace` in `WorldGenerator.cpp:36-43`; `GenerateTerrain` is rewritten to use it
- **New `TreeGenerator`** (`src/services/world_generator/TreeGenerator.h/.cpp`, style of `OreGenerator`) — stateless, per-column deterministic, called from `GenerateTerrain`; 8-thread safe
- **Deterministic tree predicate**: single scalar `treeScore = chance × density`; a tree spawns only if `treeScore > TREE_SCORE_THRESHOLD` AND it is the strict local maximum in a 5×5 window (trunk spacing ≥ 3 blocks); density comes from a low-frequency `forestFBM` (noise clustering, no biome system)
- **Canopy = ellipsoid** (`R_h=2, R_v=1.5`), leaves placed only on AIR
- **Surface guard**: slope check (`|Δheight| ≤ SLOPE_MAX`) baked into the shared predicate — identical on both sides of chunk borders (no orphaned canopies); water guard omitted (terrain heights are [32,96], water is unreachable)
- **New blocks** in `data/registry/items.csv`: `0:10:11:2 oak_log` (packed `0x5802`), `0:10:11:3 oak_leaves` (`0x5803`)
- **Crafting recipe `oak_log_to_planks`** — 1 log → 4 planks, **server-authoritative only**:
  - `data/recipes/crafting_table.yaml` (by name `oak_log`)
  - Client preview goes through the existing server-driven path: `CraftingGrid::Recalc` fires `onGridChanged_` → `ServerRecipeDB` queries `recipe.check` through the gateway → `ApplyServerResult` fills the grid result → craft button activates (`ClientCraftingWindow.cpp:128`). No client-side recipe table — `ClientRecipeDB`/`ClientMachineRecipeDB` are already removed in this changeset
- **Block colors** in `ChunkMeshBuilder::GetBlockColor`: oak_log/oak_leaves colors + fix existing dirt/grass bug (real ids `0:0:7`/`0:0:8` were mapped to wrong ids `0:0:2`/`0:0:3`, terrain rendered white; `0:0:2`/`0:0:3` are actually cobblestone/sand per `items.csv`)
- **Tests**: determinism, chunk borders (horizontal + vertical), trunk spacing, trunk-on-ground, non-AIR invariant, no-orphan-canopy, recipes (server-side YAML resolution + `RecipeManagerService` query test), `static_assert` of packed ids
- **Existing drop path untouched**: breaking `oak_log` already gives the block itself via `SetBlockCASHandler` (`onGiveItem(..., broken_block, 1, -1)`) — no changes in SimulationCore

## Impact

- Affected specs: `tree-generation` (new, ADDED), `player-interaction` (MODIFIED — world generation scenario now includes trees)
- Affected code:
  - `src/services/world_generator/WorldGenerator.cpp/.h` (height formula → `SurfaceHeights`, call `TreeGenerator`)
  - `src/services/world_generator/SurfaceHeights.h/.cpp` (new)
  - `src/services/world_generator/TreeGenerator.h/.cpp` (new)
  - `src/services/world_generator/CMakeLists.txt` (add new sources)
  - `data/registry/items.csv` (2 new blocks)
  - `data/recipes/crafting_table.yaml` (1 new recipe)
  - `src/services/game_client/Render/ChunkMeshBuilder.cpp` (colors)
  - `src/services/game_client/tests/` (static_assert of packed ids)
  - `src/services/game_client/Crafting/ServerRecipeDB.h/.cpp` (already landing as part of the client recipe-query refactor; recipe flows through it, no client table change needed)
  - world_generator tests (new `worldgeneratord_test` target)
- Data reset required for visual verification: old chunks are never regenerated → `rm -rf ./chunkdb`

## Non-Goals (for this change)

- Biome system / `BiomeProvider` (Phase 2)
- Spruce/birch variants (Phase 2)
- Water level & coastal trees (water currently unreachable — Phase 2)
- Honest surface block check (`blockAt == GRASS/DIRT` needs 3×3 neighbor generation, conflicts with independent parallel chunk gen — Phase 2)
- Drop tables (leaves → sapling chance, planting/growth) — Phase 2
- Semi-transparent / alpha-cutout leaves (separate mesh path) — Phase 2
- `trees.json` config (Phase 2, by analogy with `ores.json`)
