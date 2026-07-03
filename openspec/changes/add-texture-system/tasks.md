## 1. Asset files
- [ ] 1.1 Create `data/textures/` directory
- [ ] 1.2 Create `data/textures/often.png` — 16×16 tile grid with stone, dirt, grass, cobble, sand, gravel, planks, etc.
- [ ] 1.3 Create `data/textures/machines.png` — furnace top/side/front, macerator, generator, crafting table, chest
- [ ] 1.4 Create `data/textures/ores.png` — iron, gold, copper, tin, uranium, quartz ore tiles
- [ ] 1.5 Create `data/textures/pipes.png` — pipe/cable connector openings, machine port indicators
- [ ] 1.6 Create `data/textures/textures.csv` — tile_id → source file + tile coords
- [ ] 1.7 Create `data/textures/block_faces.csv` — block_id → face-to-tile mapping + transparency

## 2. Atlas loader
- [ ] 2.1 Add `lodepng` include path / link to `RenderLib/CMakeLists.txt`
- [ ] 2.2 Implement CSV parser helper in `TextureAtlas.cpp` (simple split + atoi, no library)
- [ ] 2.3 Implement `TextureAtlas::LoadTextureRegistry()` — parse `textures.csv`, build source file list
- [ ] 2.4 Implement `TextureAtlas::LoadBlockFaceRegistry()` — parse `block_faces.csv`, fill `s_tileX/Y[256][6]` and `s_transparent[256]`
- [ ] 2.5 Implement `TextureAtlas::LoadSourcePNG(filename, pixels, atlasW, atlasH)` — decode via `lodepng::decode()`, copy tiles into atlas pixel buffer
- [ ] 2.6 Rewrite `TextureAtlas::Init()` — call registry parsers, load source PNGs, create GPU texture
- [ ] 2.7 Remove procedural `FillTileFlat`/`FillTileChecker` functions

## 3. Transparency support
- [ ] 3.1 Add `s_transparent[256]` array to `TextureAtlas` (static, read by renderer)
- [ ] 3.2 Add `TextureAtlas::IsTransparent(uint16_t blockId)` accessor
- [ ] 3.3 Modify `RenderScene::Render()` — collect opaque and transparent mesh draw calls separately
- [ ] 3.4 Submit opaque pass with `BGFX_STATE_DEFAULT`
- [ ] 3.5 Submit transparent pass with `BGFX_STATE_BLEND_*` + `BGFX_STATE_DEPTH_WRITE`

## 4. Validation
- [ ] 4.1 Verify atlas pixels match expected colors via visual inspection (run client, look at terrain)
- [ ] 4.2 Verify transparency renders correctly (place crafting table with alpha regions)
- [ ] 4.3 Verify missing texture fallback (remove a CSV entry, confirm checker pattern appears)
- [ ] 4.4 Verify `bgfx::isValid` handles on shutdown/re-init cycle
