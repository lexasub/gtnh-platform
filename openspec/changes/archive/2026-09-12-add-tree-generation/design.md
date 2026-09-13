# Design: Oak Tree Generation

## Context

Chunks are generated independently on 8 worker threads (`GenerationQueue.h:25`) with no inter-chunk communication and no shared mutable state. A tree canopy (radius 2) and the 5×5 local-maximum window cross chunk borders, so both chunks must agree on the same blocks. The height formula (`64 + baseFBM*12 + contFBM*20`, range [32,96]) currently lives in a file-local `anonymous namespace` (`WorldGenerator.cpp:36-43`) and is not exported — any second consumer must not re-implement it (drift → floating trunks, broken vertical borders). Water is unreachable in this terrain (filled only when `worldY < 0`, i.e. never), so water guards are dead code.

## Goals / Non-Goals

- Goals: deterministic per-column oak generation; no inter-chunk state; shared single-source height formula; survival-viable `oak_planks` (blocks + recipe working client AND server); correct client colors; tests proving determinism, border consistency, and no orphan canopies.
- Non-Goals: biomes, spruce/birch, water level, honest `blockAt` surface check, sapling/drop tables, alpha-cutout leaves, `trees.json` config (all Phase 2).

## Decisions

### Decision 1: Per-column determinism via a single tree predicate

**Why**: 8 independent threads ⇒ everything must be a pure function of `(x, z, seed)` — the same pattern as `hashRegion` for ores (`OreGenerator.cpp:24`).

One scalar `treeScore(x,z) = chance × density` where:
- `chance = (hashTree(x,z) & 0xFFFF) / 65535.0f` — uniform hash-derived probability
- `density = 0.5 + 0.5*forestFBM(x,z)` — low-frequency noise clustering (forest patches, no biome system)

A tree spawns iff `treeScore > TREE_SCORE_THRESHOLD (0.12f)` AND its score is the strict local maximum in a 5×5 window. Density gates candidates **through the same number that local-max compares** — no split criteria (review finding: three competing predicate definitions in an earlier plan revision). Trunk spacing ≥ 3 blocks (Chebyshev) so canopies don't merge into a solid roof. Effective density ≈ 1 trunk per 5×5 window inside a dense patch — expected, acceptable behavior.

Alternatives considered: hard threshold on noise (rejected — sharp forest walls), separate candidate + density gates (rejected — local-max must compare the same criterion that density gates).

### Decision 2: `SurfaceHeights` — single owner of the height formula (mandatory, not optional)

**Why**: `TreeGenerator` needs a 36×36 height grid (chunk + margin 2), but FBM nodes are file-local and the formula would otherwise be duplicated → any drift puts trunks in the air/underground and breaks vertical chunk borders (both chunks must compute identical surfaces for a trunk crossing y=baseY+32).

`SurfaceHeights` owns `baseFBM_`/`contFBM_` (moved out of the anonymous namespace), exposes `fill(out, size, baseX, baseZ)` + `at(x,z)`. `GenerateTerrain` and `TreeGenerator` both consume it; the 32×32 terrain grid and the 36×36 tree grid come from the same source. One formula in the codebase.

### Decision 3: 36×36 precomputed grids (performance)

Naive canopy pass = 32×32×5×5 FBM calls per chunk — kills the generation queue. Precompute once:
- `hmap_[36×36]` — 1296 columns × 2 FBM = 2592 samples (margin 2 covers canopy radius + local-max window)
- `scoreMap_[36×36]` — 1296 × (1 hash + 1 forestFBM)
- `treeMap_[36×36]` — `isTreeAt ∧ slopeOK`, ~32k comparisons

Trunk and canopy passes read the grids O(1). `treeMap_` is the **only** source of truth for both passes → consistency is structural, not by convention.

### Decision 4: Ellipsoid canopy

`inCanopy(dx,dz,wy,centerY): (dx²+dz²)/4 + (wy−centerY)²/2.25 ≤ 1`, center at `T−1` (trunk top minus one), trunk column excluded, leaves written only into AIR. Pure math, no 3D masks, deterministic, tunable radii. Classic wide oak silhouette.

### Decision 5: Slope guard lives inside the shared predicate; water guard removed

Slope check (`|hmap_[p] − hmap_[neighbor]| ≤ SLOPE_MAX (1.5f)`) is baked into `treeMap_` so trunks AND canopies use the identical criterion — a canopy can only exist where its trunk exists (no orphaned canopies on steep slopes, inside a chunk or across borders). Heights are a pure coordinate function ⇒ the check is symmetric across chunk borders (review requirement: any surface check must be identical on both sides or absent).

Water guard is **removed**: terrain heights are [32,96] and water is filled only at `worldY < 0` (`WorldGenerator.cpp:117-118`) — unreachable, the guard was dead code. A real water level is Phase 2. Terrain surface blocks are only STONE/DIRT/GRASS (no sand), so trees are always on grass/dirt; steep slopes are filtered by the slope guard. Honest `blockAt == GRASS` requires generating 3×3 neighboring chunks before trees — conflicts with independent parallel generation, explicitly deferred to Phase 2.

### Decision 6: Recipe defined in ONE place — server YAML only

The client no longer stores recipes: the `ClientRecipeDB`/`ClientMachineRecipeDB` hardcoded tables are replaced by `ServerRecipeDB` (server-sourced, LRU-cached), which queries `RecipeManagerService` through the gateway (`recipe.check`). `CraftingGrid::Recalc` fires `onGridChanged_` (no `MatchGrid` call), the grid result is filled asynchronously via `ApplyServerResult`, and the craft button activates when `grid_.GetResult().item_id != 0` (`ClientCraftingWindow.cpp:128`). Therefore `oak_log → oak_planks ×4` needs exactly one definition:

- `data/recipes/crafting_table.yaml` (by name; numeric legacy ids are known-broken, see `generator.yaml` `{item: 13}`)

`MatchGrid`/`kRecipes` are dead code in this changeset (only the test still references them); the YAML recipe is the single source of truth for both server craft validation and the client preview.

### Decision 7: New blocks `0:10:11:2/3`

`0:10:11:*` is the BASE misc sub-prefix (0=chest, 1=crafting_table; payloads 2,3 free). Packed: segments `0`,`10`,`11` = bits `01011` (5 bits) ⇒ `prefix=11, shift=11` ⇒ `pack("0:10:11:2") = 0x5802`, `pack("0:10:11:3") = 0x5803`. No collisions (verified by computation + compile-time `static_assert`). Both land in `CAT_BASE` — existing pipeline (simcore, client, inventory) treats them as ordinary blocks.

### Decision 8: Constants (defined up front)

`SEED_TREES = 0x5EED` (hashTree offset — must NOT overlap ore `hashRegion` seed `SEED+12345`, else tree and ore distributions correlate), `TREE_SCORE_THRESHOLD = 0.12f`, `SLOPE_MAX = 1.5f`, `MAX_TREE_H = 7` (trunk `4 + (hash>>8)%4`), `FOREST_SCALE = 0.004f` (patches ~250 blocks), `BLOCK_LOG`/`BLOCK_LEAVES` as `constexpr ItemId::pack(...)` (same style as `WorldGenerator.cpp:20-24`; `ItemId::pack` is fully `constexpr`, already proven in a switch statement at `ChunkMeshBuilder.cpp:16-18`).

## Risks / Trade-offs

- **Height formula drift** (trunks floating / vertical borders torn) → `SurfaceHeights` single source + vertical-border test 6.4.
- **Orphaned canopies** (trunk pass rejects a slope, canopy pass doesn't) → slope guard inside the shared `treeMap_` + no-orphan test 6.8.
- **Correlated tree/ore distributions** → separate `SEED_TREES` offset; not directly tested, cheap to change.
- **Client/server recipe mismatch** → single server-side YAML source (`RecipeManagerService`); client preview uses `recipe.check` through the gateway, so server remains authoritative.
- **Perf regression on generation queue** → 36×36 precomputation (~5.3k noise samples + ~32k comparisons per chunk), early exit for chunks far from the surface.
- **Old chunks never regenerate** (`./chunkdb` LMDB) → visual verification requires world reset; noted in tasks 7.3.
- **Density limited by local-max window** (≈1 trunk / 5×5 in dense patches) → accepted; expected "soft clusters" behavior, tunable via `TREE_SCORE_THRESHOLD`.

## Migration Plan

1. Add `SurfaceHeights`, rewrite `GenerateTerrain` (formula moves, behavior unchanged for non-tree terrain)
2. Add blocks to `items.csv` (2 rows; additive, no existing id changes)
3. Add YAML recipe (additive; client preview flows through existing `ServerRecipeDB` → `recipe.check` path)
4. Add `TreeGenerator` + CMake sources; wire into `GenerateTerrain`
5. Fix `GetBlockColor` (add log/leaves; correct dirt/grass ids)
6. Tests (worldgeneratord_test + client tests), build, ctest
7. Visual verification after `rm -rf ./chunkdb`; rollback = revert files (terrain generation is deterministic per seed, no DB migration needed)

## Open Questions

- None blocking. Tuning knobs (`TREE_SCORE_THRESHOLD`, `SLOPE_MAX`, `FOREST_SCALE`) are constants with sensible defaults; visual tuning after first world inspection.
