## Context

GameClient renders blocks via a 256×256 RGBA8 texture atlas (16×16 tiles). Currently all tiles are procedurally generated in C++ with hardcoded colors. The README lists "Assets — Textures, models, sprites for items, blocks, and machines" as needing contribution.

`lodepng` is already available in `third_party/bgfx.cmake/bimg/3rdparty/lodepng/`. `nlohmann_json` is linked but user prefers CSV (matching existing `data/registry/*.csv` convention).

## Goals / Non-Goals

**Goals:**
- Load textures from PNG files into the atlas
- Map `[blockId][face]` → tile via human-editable CSV files
- Support transparency (alpha blend) for selected blocks
- Keep the single-GPU-texture atlas approach (no shader/vertex changes)
- Zero rendering pipeline changes outside atlas loading + blend state

**Non-Goals:**
- No animated textures
- No per-block individual PNG loading at runtime (all composited into atlas at init)
- No texture pack switching at runtime
- No mipmap generation (deferred)
- No texture compression format selection (RGBA8 for now)

## Decisions

### Atlas compositing strategy
**Decision**: Load source PNG packs at `TextureAtlas::Init()`, copy tiles into a single RGBA8 pixel buffer, create one `bgfx::createTexture2D()`.

Multiple source files (often.png, machines.png, etc.) each contain 16×16 tiles arranged in a grid. `textures.csv` specifies which tile from which source goes to which atlas slot.

**Alternatives considered:**
- Per-block individual PNGs → N file reads instead of 3-5, higher I/O overhead
- Pre-baked single atlas PNG → art teams can't work in parallel on separate packs
- KTX/DDS → lodepng simpler, PNG is universal for pixel-art 16×16 tiles

### CSV format
**Decision**: Plain CSV matching existing `data/registry/` convention. Parsed with a simple split + `atoi` — no CSV library needed. Files are small (<100 rows).

### Transparency
**Decision**: Binary `transparent` column in `block_faces.csv`. `RenderScene` collects opaque and transparent meshes separately, submits transparent batch with `BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA)` after opaque pass.

Per-face alpha (e.g. pipe opening transparent but sides opaque) deferred — the flag is per-block for MVP.

### Lazy loading
**Decision**: NOT lazy. All source PNGs loaded at `Init()`. Reason: atlas is 256 KB (256×256×4 bytes), source packs are smaller. Loading 3-5 files at startup is negligible. Lazy loading adds complexity (thread safety, missing-tile fallback) with no measurable benefit at current scale.

## Risks / Trade-offs

- **PNG decode at init blocks startup** → decode is sub-millisecond for 16×16 tiles, negligible
- **CSV parse errors** → logs error + falls back to magenta checker tile (visible but non-crashing)
- **Block ID mismatch** → texture system uses the same `block & 0xFF` → tileId mapping as current procedural code. If IDs change, CSV must be updated

## Open Questions

- Should `textures.csv` and `block_faces.csv` be merged into one file? (separate keeps concerns clean — one defines tile→source mapping, other defines block→tile mapping)
- Should `RenderScene` sort transparent meshes back-to-front? (correctness depends on use case — for machine windows and crafting tables, simple alpha test + depth-write-off may suffice)
