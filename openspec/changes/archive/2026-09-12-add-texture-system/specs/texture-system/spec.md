## ADDED Requirements

### Requirement: Texture Source Files
The system SHALL load block textures from PNG source files organized in separate packs under `data/textures/`.

#### Scenario: Source file structure
- **GIVEN** the `data/textures/` directory exists
- **WHEN** `TextureAtlas::Init()` runs
- **THEN** it SHALL locate source PNG files (often.png, machines.png, ores.png, pipes.png)
- **AND** each source PNG SHALL contain a tile grid (tiles assumed to be 16×16 pixels)
- **AND** any missing source file SHALL log a warning and continue with available files

#### Scenario: Multiple source packs
- **GIVEN** `textures.csv` references `machines.png` and `ores.png`
- **WHEN** the atlas is built
- **THEN** tiles from both files SHALL appear in the atlas at their assigned tile IDs

### Requirement: Texture Registry (textures.csv)
The system SHALL read a CSV file `data/textures/textures.csv` that maps tile IDs to source file + tile coordinates.

#### Scenario: CSV format
- **GIVEN** `textures.csv` exists
- **WHEN** parsed
- **THEN** each row SHALL contain: `tile_id,filename,src_x,src_y`
- **AND** `tile_id` SHALL be a 0-255 integer
- **AND** `filename` SHALL be relative to `data/textures/`
- **AND** `src_x`, `src_y` SHALL be tile-grid coordinates (not pixel coordinates)
- **AND** rows with duplicate `tile_id` SHALL log a warning; the last row wins

#### Scenario: Missing tile fallback
- **GIVEN** a tile ID has no entry in `textures.csv` or its source file is missing
- **WHEN** the atlas is built
- **THEN** that tile SHALL be filled with the magenta/black checker pattern (visible error marker)

### Requirement: Block-Face Mapping (block_faces.csv)
The system SHALL read a CSV file `data/textures/block_faces.csv` that maps block IDs and faces to tile IDs.

#### Scenario: CSV format
- **GIVEN** `block_faces.csv` exists
- **WHEN** parsed
- **THEN** each row SHALL contain: `block_id,face_px,face_nx,face_py,face_ny,face_pz,face_nz,transparent`
- **AND** each `face_*` column SHALL be a tile_id (0-255)
- **AND** `transparent` SHALL be 0 (opaque) or 1 (alpha blended)

#### Scenario: Per-face textures
- **GIVEN** block ID 3 with `face_py=3, face_ny=2, face_*=1`
- **WHEN** `ChunkMeshBuilder::Build()` calls `TextureAtlas::GetUV(3, face)`
- **THEN** face 2 (top, +Y) SHALL return UV for tile 3
- **AND** face 3 (bottom, -Y) SHALL return UV for tile 2
- **AND** all other faces SHALL return UV for tile 1

#### Scenario: Default fallback
- **GIVEN** a block ID has no row in `block_faces.csv`
- **WHEN** the system looks up its face textures
- **THEN** all 6 faces SHALL default to tile_id = block_id (matching current procedural behavior)
- **AND** transparency SHALL default to 0 (opaque)

### Requirement: Atlas Compilation
The system SHALL composite all referenced source PNG tiles into a single GPU texture atlas at initialization.

#### Scenario: Init flow
- **GIVEN** `TextureAtlas::Init(16)` is called
- **WHEN** initialization completes
- **THEN** the atlas SHALL be a 256×256 RGBA8 texture (16×16 tiles of 16×16 pixels each)
- **AND** `TextureAtlas::GetTextureHandle()` SHALL return a valid `bgfx::TextureHandle`
- **AND** `TextureAtlas::GetUV(tileId, face)` SHALL return correct UV coordinates for all assigned tiles

#### Scenario: Idempotent init
- **GIVEN** `TextureAtlas::Init()` has been called once
- **WHEN** called again
- **THEN** it SHALL return immediately without re-allocating or leaking the previous texture

#### Scenario: Shutdown cleanup
- **GIVEN** `TextureAtlas::Shutdown()` is called
- **WHEN** after `bgfx::destroy()` on the atlas texture
- **THEN** `GetTextureHandle()` SHALL return `BGFX_INVALID_HANDLE`
- **AND** subsequent `Init()` SHALL re-create the atlas cleanly

### Requirement: Transparency Rendering
Blocks marked as transparent SHALL render with alpha blending, rendered in a separate pass after opaque geometry.

#### Scenario: Transparent render pass
- **GIVEN** a block with `transparent=1` in `block_faces.csv` exists in the visible frustum
- **WHEN** `RenderScene::Render()` processes visible meshes
- **THEN** opaque blocks SHALL render first with `BGFX_STATE_DEFAULT`
- **AND** transparent blocks SHALL render after with `BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA) | BGFX_STATE_DEPTH_WRITE`

#### Scenario: Mixed opaque/transparent chunks
- **GIVEN** a chunk contains both opaque and transparent blocks
- **WHEN** the chunk mesh is built
- **THEN** `ChunkMeshBuilder::Build()` SHALL produce separate vertex/index data for opaque and transparent faces (or the renderer SHALL split them by block flag)

### Requirement: Backward Compatibility
The file-based texture system SHALL produce visually identical output for block IDs 1, 2, 3 as the current procedural system, when equivalent CSV entries are provided.

#### Scenario: Default textures match procedural
- **GIVEN** `textures.csv` defines tile 0=gray, tile 1=brown, tile 2=green, tile 3=green
- **AND** `block_faces.csv` maps block 1→tile 0, block 2→tile 1, block 3→{top=tile2, sides=tile3, bottom=tile1}
- **WHEN** `TextureAtlas::Init()` completes
- **THEN** the atlas SHALL contain the same tile colors at the same positions as the current procedural code
