## ADDED Requirements

### Requirement: Oak Tree Generation
The system SHALL generate oak trees during chunk generation, deterministically as a pure function of block coordinates and world seed, with no inter-chunk communication.

#### Scenario: Generation is deterministic per seed
- **GIVEN** two chunks generated with the same world seed
- **WHEN** `WorldGenerator::GenerateTerrain` completes for both
- **THEN** the trunk and leaf blocks at identical world coordinates SHALL be identical
- **AND** a chunk generated a second time SHALL produce byte-identical block data

#### Scenario: Trunks are spaced apart
- **GIVEN** a generated 3×3 chunk area
- **WHEN** trunk columns are collected
- **THEN** no two trunks SHALL be closer than 3 blocks (Chebyshev distance)
- **AND** this SHALL follow from a strict local-maximum rule over a 5×5 window of a single tree score derived from the coordinate hash and a low-frequency forest density noise

#### Scenario: Canopy crosses chunk borders consistently
- **GIVEN** a tree whose canopy (radius 2) overlaps the border between chunks `(cx,cz)` and `(cx+1,cz)`
- **WHEN** both chunks are generated
- **THEN** the same leaf blocks SHALL be placed at the same world coordinates by both chunks
- **AND** there SHALL be no cut-off or missing canopy half at the border

#### Scenario: Trunk crosses vertical chunk borders continuously
- **GIVEN** a tree whose trunk spans the y-plane between chunks `(cx,cy)` and `(cx,cy+1)`
- **WHEN** both chunks are generated
- **THEN** the combined trunk SHALL be a continuous column with no gap or overlap
- **AND** both chunks SHALL compute the trunk from the identical single height formula

#### Scenario: No trees on steep slopes
- **GIVEN** a column whose height differs from a neighboring column by more than `SLOPE_MAX`
- **WHEN** tree placement is evaluated
- **THEN** no trunk SHALL be placed at that column
- **AND** no canopy SHALL reference a tree at that column, so no leaves appear without a trunk

#### Scenario: Leaves only replace air
- **GIVEN** a leaf position that is occupied by a non-air block (stone, ore, terrain, trunk)
- **WHEN** the canopy pass runs
- **THEN** the existing block SHALL NOT be overwritten

#### Scenario: Trees cluster softly
- **GIVEN** the forest density noise value at a column
- **WHEN** tree score is computed
- **THEN** the density SHALL scale the tree probability continuously rather than act as a binary forest/not-forest threshold

### Requirement: Oak Log and Leaves Blocks
The system SHALL register `oak_log` and `oak_leaves` as ordinary base blocks, obtainable by breaking trees.

#### Scenario: Blocks registered with stable ids
- **GIVEN** `data/registry/items.csv`
- **WHEN** the registry loads
- **THEN** `oak_log` SHALL resolve to `ItemId::pack("0:10:11:2") == 0x5802`
- **AND** `oak_leaves` SHALL resolve to `ItemId::pack("0:10:11:3") == 0x5803`
- **AND** neither id SHALL collide with any existing item id (compile-time `static_assert` in client tests)

#### Scenario: Breaking a log drops the log
- **GIVEN** a player breaks an `oak_log` block via the existing CAS break path
- **WHEN** `SetBlockCASHandler` succeeds
- **THEN** the player inventory SHALL receive `oak_log` through the existing `onGiveItem` path
- **AND** no drop-table changes SHALL be required

### Requirement: Oak Planks Crafting Recipe
The system SHALL provide a crafting recipe converting 1 `oak_log` into 4 `oak_planks`, with the server as the single authority and the client preview resolved through the server-driven recipe query path.

#### Scenario: Recipe resolves on the server
- **GIVEN** `data/recipes/crafting_table.yaml` contains `oak_log_to_planks` referencing items by name
- **WHEN** RecipeManager loads recipes and validates a `CraftRequest` for a grid with one `oak_log`
- **THEN** the recipe SHALL match and craft 4 `oak_planks`
- **AND** the recipe SHALL use the name `oak_log` (never a numeric id)

#### Scenario: Client craft preview comes from the server
- **GIVEN** the player places one `oak_log` in the crafting grid slot 0 (top-left)
- **WHEN** `CraftingGrid::Recalc` fires `onGridChanged_` and `ServerRecipeDB` queries the gateway (`recipe.check`)
- **THEN** the grid result SHALL be `oak_planks ×4`, filled via `ApplyServerResult`
- **AND** the craft button SHALL activate (`grid_.GetResult().item_id != 0`) and send `SendCraftRequest`
- **AND** the client SHALL NOT require any client-side recipe table (`ClientRecipeDB`/`ClientMachineRecipeDB` are removed)

#### Scenario: Server is authoritative over the preview
- **GIVEN** a grid that matches no server-side recipe
- **WHEN** `ServerRecipeDB` receives the `recipe.check` response
- **THEN** the grid result SHALL be empty (no craftable output)
- **AND** the craft button SHALL stay disabled

### Requirement: Block Rendering Colors
The client SHALL render `oak_log` and `oak_leaves` with distinct colors and SHALL render dirt and grass with their correct colors.

#### Scenario: Log and leaves have colors
- **GIVEN** a chunk mesh containing `oak_log` and `oak_leaves` blocks
- **WHEN** `ChunkMeshBuilder::GetBlockColor` is called for them
- **THEN** `oak_log` SHALL render brown and `oak_leaves` SHALL render green

#### Scenario: Dirt and grass render correctly
- **GIVEN** terrain blocks `0:0:7` (dirt) and `0:0:8` (grass)
- **WHEN** `ChunkMeshBuilder::GetBlockColor` is called
- **THEN** dirt SHALL render brown and grass SHALL render green
- **AND** the incorrect legacy mappings (`0:0:2`/`0:0:3`) SHALL be removed so terrain no longer renders white
