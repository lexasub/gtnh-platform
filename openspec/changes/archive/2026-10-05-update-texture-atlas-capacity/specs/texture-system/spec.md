## MODIFIED Requirements

### Requirement: Texture Registry (textures.csv)
The system SHALL read a CSV file `data/textures/textures.csv` that maps tile IDs to source file + tile coordinates.

#### Scenario: CSV format
- **GIVEN** `textures.csv` exists
- **WHEN** parsed
- **THEN** each row SHALL contain: `tile_id,filename,src_x,src_y`
- **AND** `tile_id` SHALL be an integer from 0 through 1023
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
- **AND** each `face_*` column SHALL be a tile ID from 0 through 1023
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
- **THEN** the atlas SHALL be a 512×512 RGBA8 texture (32×32 tiles of 16×16 pixels each)
- **AND** `TextureAtlas::GetTextureHandle()` SHALL return a valid `bgfx::TextureHandle`
- **AND** `TextureAtlas::GetUV(tileId, face)` SHALL return correct UV coordinates for all assigned tiles from ID 0 through 1023

#### Scenario: Idempotent init
- **GIVEN** `TextureAtlas::Init()` has been called once
- **WHEN** called again
- **THEN** it SHALL return immediately without re-allocating or leaking the previous texture

#### Scenario: Shutdown cleanup
- **GIVEN** `TextureAtlas::Shutdown()` is called
- **WHEN** after `bgfx::destroy()` on the atlas texture
- **THEN** `GetTextureHandle()` SHALL return `BGFX_INVALID_HANDLE`
- **AND** subsequent `Init()` SHALL re-create the atlas cleanly

#### Scenario: Self-merge overlay allocation
- **GIVEN** a `textures_merge.csv` row has `composite_id` equal to `overlay_tile_id`
- **WHEN** the atlas loads the overlay source and creates the composite
- **THEN** it SHALL retain the source filename and source-cell coordinates for blending
- **AND** it SHALL allocate only one physical atlas slot for the final composite
- **AND** ordinary merges with distinct composite and overlay IDs SHALL retain their standalone overlay slot

## ADDED Requirements

### Requirement: Atlas Capacity Safety
The runtime and texture authoring tools SHALL reject invalid tile IDs and physical slot exhaustion without wrapping or writing outside the atlas.

#### Scenario: Highest valid tile ID
- **GIVEN** a tile with ID 1023 is present in the texture registry
- **WHEN** the atlas assigns its slot
- **THEN** the slot SHALL be within the 32×32 atlas grid
- **AND** its UV coordinates SHALL remain within the normalized 0..1 range

#### Scenario: Out-of-range logical tile ID
- **GIVEN** a texture, merge, block-face, item-icon, or transport reference uses tile ID 1024 or greater
- **WHEN** the scanner or runtime parses the registry
- **THEN** it SHALL reject or skip that row safely
- **AND** it SHALL NOT index per-tile storage or write outside the atlas

#### Scenario: Physical atlas exhaustion
- **GIVEN** all 1024 physical atlas slots have been allocated
- **WHEN** another source or composite slot is requested
- **THEN** the runtime SHALL reject that allocation safely without counter wrap or memory overwrite

#### Scenario: Self-merge slot accounting
- **GIVEN** a merge registry includes rows where `composite_id` equals `overlay_tile_id`
- **WHEN** the atlas allocates source and composite slots
- **THEN** a self-merge overlay SHALL retain its source-image metadata but SHALL NOT consume a redundant standalone slot
- **AND** each composite SHALL consume exactly one physical slot
- **AND** ordinary source tiles and non-self-merge overlays SHALL retain their own slots
