# Tasks: Add Oak Tree Generation (survival oak planks)

## 1. SurfaceHeights — shared height formula

- [ ] 1.1 Create `src/services/world_generator/SurfaceHeights.h/.cpp`: owns `baseFBM_`/`contFBM_` (moved from file-local anonymous namespace `WorldGenerator.cpp:36-43`), API `fill(float* out, int size, int baseX, int baseZ)` + `at(x,z)`, formula `BASE_HEIGHT(64) + baseNoise*12 + contNoise*20`
- [ ] 1.2 Rewrite `WorldGenerator::GenerateTerrain` (`WorldGenerator.cpp:84-141`) to use `SurfaceHeights`; delete the duplicated FBM nodes and inline formula
- [ ] 1.3 Add both files to `world_generator/CMakeLists.txt` (`add_library(worldgeneratord ...)`, `CMakeLists.txt:12-16`)

## 2. TreeGenerator — deterministic oak trees

- [ ] 2.1 Create `src/services/world_generator/TreeGenerator.h/.cpp` (style of `OreGenerator`), stateless: only `const uint32_t m_seed_`, thread_local buffers, object on stack
- [ ] 2.2 Predicate: `hashTree(x,z)` (seed offset `SEED_TREES = 0x5EED` — must NOT collide with ore `hashRegion` seed), `density(x,z)` (`forestFBM`, freq `FOREST_SCALE = 0.004f`, 3 octaves), single `treeScore = chance × density`, `isTreeAt` = `score > TREE_SCORE_THRESHOLD (0.12f)` ∧ strict local max in 5×5
- [ ] 2.3 Precompute thread_local grids `hmap_[36×36]` (from `SurfaceHeights`), `scoreMap_[36×36]`, `treeMap_[36×36]` (`isTreeAt ∧ slopeOK`, slope = `|Δh| ≤ SLOPE_MAX (1.5f)` among 4 neighbors) — margin 2, the ONLY source for trunks AND canopies
- [ ] 2.4 Trunk pass: for columns of the current chunk with `treeMap_ == true`, place `BLOCK_LOG` (`ItemId::pack("0:10:11:2")` constexpr, like `WorldGenerator.cpp:20-24`) from `surface+1` to `surface+h` (h = `4 + (hash>>8)%4` → 4..7), clamped to `[baseY, baseY+32)`
- [ ] 2.5 Canopy pass: for each column, check `treeMap_` trees within radius 2; ellipsoid `inCanopy` (`R_h=2, R_v=1.5`, center `T-1`, trunk excluded), write `BLOCK_LEAVES` (`pack("0:10:11:3")`) ONLY where block is AIR
- [ ] 2.6 Early exit: if `max(hmap_) < baseY || min(hmap_) > baseY+32`, return without work
- [ ] 2.7 Integrate in `GenerateTerrain` (after `oreGen.generateOres`): `TreeGenerator treeGen(SEED); treeGen.generateTrees(c, surf, baseX, baseY, baseZ, CHUNK_SIZE);`

## 3. Registry — new blocks

- [ ] 3.1 Append to `data/registry/items.csv` BASE wood section: `0:10:11:2,oak_log,,0` and `0:10:11:3,oak_leaves,,0` (free payloads under `0:10:11`; 0=chest, 1=crafting_table)
- [ ] 3.2 static_assert id invariants in `game_client/tests/` (e.g. `test_client.cpp`): `pack("0:10:11:2") == 0x5802u`, `pack("0:10:11:3") == 0x5803u`, no collision with `pack("0:10:11:0")`/`pack("0:10:11:1")`

## 4. Crafting recipe — server YAML only

- [ ] 4.1 Add `oak_log_to_planks` to `data/recipes/crafting_table.yaml`: 1 `oak_log` → 4 `oak_planks`, pattern `[[oak_log, ~, ~], [~, ~, ~], [~, ~, ~]]`, `duration: 100`, `min_tier: 0`, `max_tier: 32767` — by NAME only, no numeric ids
- [ ] 4.2 No client recipe table change: client preview resolves via `ServerRecipeDB` → `recipe.check` (already wired; `ClientRecipeDB`/`ClientMachineRecipeDB` are removed in the client recipe-query refactor)
- [ ] 4.3 Recipe resolution test in `simulation_core/test/test_recipe_manager.cpp` (pattern `test_recipe_manager_queries`): `findRecipesForItem(kLog, ...)` finds `oak_log_to_planks` and `CheckRecipeReq` for a `{log,0,0,...}` grid resolves to `oak_planks ×4`

## 5. Client rendering

- [ ] 5.1 `ChunkMeshBuilder::GetBlockColor` (`ChunkMeshBuilder.cpp:14-21`): add `case ItemId::pack("0:10:11:2") → 0xFF8B5A2B` (log), `case ItemId::pack("0:10:11:3") → 0xFF228B22` (leaves)
- [ ] 5.2 Fix existing bug: `0:0:7` dirt → brown, `0:0:8` grass → green; remove/repair wrong `0:0:2` (cobblestone) and `0:0:3` (sand) cases

## 6. Tests (world_generator)

- [ ] 6.1 Add `worldgeneratord_test` target (pattern: `simcored_test`, `simulation_core/CMakeLists.txt:213`)
- [ ] 6.2 Determinism: `generateTrees` twice with same seed → identical chunks
- [ ] 6.3 Horizontal border: `(cx,cz)` + `(cx+1,cz)` — columns x=31/32 have consistent trunk/leaves, no canopy break
- [ ] 6.4 Vertical border: `(cx,cy)` + `(cx,cy+1)` — trunk crossing y=baseY+32 is continuous (catches height-formula drift)
- [ ] 6.5 Trunk spacing: no two trunks within < 3 blocks (Chebyshev) across a 3×3 chunk area
- [ ] 6.6 Trunk on ground: block under trunk is non-AIR; trunk starts at `surface+1`
- [ ] 6.7 Non-AIR invariant: chunk generated with ores — non-AIR block count is unchanged by `generateTrees`
- [ ] 6.8 No orphan canopies: in a 3×3 chunk area every column containing leaves has a trunk (from `treeMap_`) within radius 2
- [ ] 6.9 Recipe YAML test (pattern `test_recipe_manager_queries` in `test_recipe_manager.cpp`): `oak_log` → `oak_planks` x4 resolves by name, and the server-side `CheckRecipeReq` handler returns it

## 7. Verification

- [ ] 7.1 Build: `cd cmake-build-debug && ninja -j5`
- [ ] 7.2 Tests: `ctest --output-on-failure -j$(nproc)`
- [ ] 7.3 Reset world: `rm -rf ./chunkdb` (old chunks never regenerate)
- [ ] 7.4 Manual: start stack (`routerd → chunkd → entitystated → gatewayd → simcored → client`), fly over terrain — forests present, chunk borders show no cut canopies
- [ ] 7.5 Manual: break a trunk → `oak_log` in inventory; place log in crafting grid → craft button active → `oak_planks` x4
- [ ] 7.6 Manual: dirt/grass no longer render white
- [ ] 7.7 `openspec validate add-tree-generation --strict`
