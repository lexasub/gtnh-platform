## ADDED Requirements

### Requirement: Multiblock Pattern Library
The system SHALL support generic multiblock pattern matching.

#### Scenario: EBF pattern detected
- **GIVEN** a player builds a 3x3x4 hollow structure with casing blocks and a heating coil layer
- **WHEN** the controller block is placed at the anchor position
- **THEN** `onBlockChanged` triggers pattern matching via `PatternRegistry::matchAll()`
- **AND** SimulationCore detects the EBF pattern
- **AND** creates a `MultiblockController` ECS entity
- **AND** publishes `sim.multiblock.created`

#### Scenario: Large Boiler pattern detected
- **GIVEN** a player builds a 3x3x4 structure with firebox bottom layer, water input hatch, and steam output hatch
- **WHEN** the controller block is placed at the anchor position
- **THEN** SimulationCore detects the Large Boiler pattern
- **AND** creates a `MultiblockController` ECS entity

#### Scenario: LCR pattern detected
- **GIVEN** a player builds a 3x3x3 casing structure with fluid input/output hatches
- **WHEN** the controller block is placed
- **THEN** SimulationCore detects the LCR pattern
- **AND** creates a `MultiblockController` ECS entity

#### Scenario: Pattern mismatch on controller placement
- **GIVEN** a controller block is placed but the surrounding blocks do not match any registered pattern
- **WHEN** `PatternRegistry::matchAll()` runs
- **THEN** no `MultiblockController` is created
- **AND** the block exists as a standalone machine (managed_externally=false)

### Requirement: EBF Multiblock Tick
The system SHALL process EBF recipes respecting heating coil tier and heat requirements.

#### Scenario: EBF processes recipe with sufficient heat
- **GIVEN** an EBF multiblock with TungstenSteel coils (max_heat=4500K)
- **AND** a recipe requiring 3000K is loaded
- **WHEN** EBFSystem ticks
- **THEN** recipe progresses
- **AND** EU/tick is consumed from energy hatch
- **AND** output items appear in output hatch slots
- **AND** muffler byproducts are auto-ejected

#### Scenario: EBF pauses when heat insufficient
- **GIVEN** an EBF multiblock with Kanhal coils (max_heat=1800K)
- **AND** a recipe requiring 3000K is loaded
- **WHEN** EBFSystem ticks
- **THEN** recipe progress is paused
- **AND** a warning is emitted via spdlog

#### Scenario: EBF heating coil determines max heat
- **GIVEN** an EBF with Kanhal coils (block_id=KANHAL_COIL)
- **WHEN** EBFSystem reads the coil layer
- **THEN** max_heat = 1800K
- **WHEN** coils are Nichrome (block_id=NICHROME_COIL)
- **THEN** max_heat = 2700K
- **WHEN** coils are TungstenSteel (block_id=TUNGSTENSTEEL_COIL)
- **THEN** max_heat = 4500K

### Requirement: Large Boiler Multiblock Tick
The system SHALL process large boiler operation: fuel burning, water→steam conversion, overheat detection.

#### Scenario: Large Boiler produces steam
- **GIVEN** a Large Boiler multiblock with coal in firebox and water in input fluid hatch
- **WHEN** LargeBoilerSystem ticks
- **THEN** fuel count decrements
- **AND** heat is generated in HeatIntakeComponent
- **AND** water is consumed from input hatch
- **AND** steam is produced to PipeNetwork via `publishNodeUpdate`

#### Scenario: Large Boiler overheats without water
- **GIVEN** a Large Boiler with fuel in firebox but no water in input hatch
- **WHEN** LargeBoilerSystem ticks
- **THEN** heat builds up in HeatIntakeComponent
- **AND** OverheatComponent is set to WARNING or CRITICAL

#### Scenario: Large Boiler multi-size heat scaling
- **GIVEN** a 1x1x1 Large Boiler pattern
- **WHEN** the boiler operates
- **THEN** heat capacity and steam output scale to minimum
- **GIVEN** a 3x3x4 Large Boiler pattern
- **WHEN** the boiler operates
- **THEN** heat capacity and steam output scale to maximum

### Requirement: LCR Multiblock Tick
The system SHALL process LCR recipes with fluid and solid inputs.

#### Scenario: LCR processes fluid+solid recipe
- **GIVEN** an LCR multiblock with sulfuric acid in fluid input hatch and iron in item input hatch
- **AND** a recipe for iron_sulfate exists
- **WHEN** LCRSystem ticks
- **THEN** RecipeManager.findRecipe(lcr_id, {iron, sulfuric_acid}) returns the recipe
- **AND** inputs are consumed
- **AND** iron_sulfate is produced to output hatch
- **AND** any fluid byproduct is routed to fluid output hatch
- **AND** EU/tick is consumed from energy hatch

#### Scenario: LCR pauses without fluid input
- **GIVEN** an LCR with a recipe requiring fluid but no fluid in input hatch
- **WHEN** LCRSystem ticks
- **THEN** recipe progress is paused
- **AND** a warning is emitted

### Requirement: Hatch Detection
The system SHALL detect and classify hatches on multiblock structures.

#### Scenario: Input/output hatches detected on EBF formation
- **GIVEN** an EBF multiblock forms with item input hatches on the left side and energy hatch on the right side
- **WHEN** `findHatches(controller_pos, pattern)` runs during pattern match
- **THEN** the left-side positions are classified as ITEM_IN
- **AND** the right-side position is classified as ENERGY
- **AND** hatch slots are mapped in the `MachineComponent` slot configuration

#### Scenario: Fluid hatches classified
- **GIVEN** a Large Boiler with fluid input (water) and fluid output (steam) hatches
- **WHEN** hatches are detected
- **THEN** the water hatch is classified as FLUID_IN
- **AND** the steam hatch is classified as FLUID_OUT

#### Scenario: Muffler hatch on top layer
- **GIVEN** an EBF with a muffler hatch on the top-center position
- **WHEN** hatches are detected
- **THEN** the top-center position is classified as MUFFLER

### Requirement: Multiblock Dissociation
The system SHALL detect and handle multiblock destruction.

#### Scenario: Anchor block broken disassembles multiblock
- **GIVEN** a multiblock exists with a controller at the anchor position
- **WHEN** the anchor block is broken (becomes air)
- **THEN** SimulationCore checks `isMultiblockAnchor(pos)` in `onBlockChanged()`
- **AND** iterates all pattern blocks from `MultiblockController.blocks[]`
- **AND** clears `mb_id` from each via ChunkStore `SetBlockMeta`
- **AND** destroys the `MultiblockController`
- **AND** ejects hatch contents
- **AND** publishes `sim.multiblock.destroyed`

#### Scenario: Non-anchor block removal removes block from controller
- **GIVEN** a multiblock exists with a casing piece
- **WHEN** the casing piece is broken (becomes air)
- **THEN** `removeBlockFromController(mb_id, pos)` removes the packed position from `MultiblockController.blocks`
- **AND** the multiblock continues to exist (controller intact)
- **AND** `mb_id` is cleared from that block's meta-layer

### Requirement: Multiblock Persistence
The system SHALL persist and restore multiblock state via EntityStateStore.

#### Scenario: Multiblock state saved on chunk unload
- **GIVEN** a multiblock with an active controller (anchor inside chunk being unloaded)
- **WHEN** ChunkStore requests unload with the anchor mb_id
- **THEN** SimulationCore serializes `MultiblockState` (controller_id, anchor, blocks, hatch data, recipe progress)
- **AND** calls `EntityStateStoreClient::SaveEntityState()` with the serialized blob
- **AND** returns `release` to ChunkStore

#### Scenario: Multiblock state restored on chunk load
- **GIVEN** a chunk is loaded that contains a multiblock anchor with saved state in EntityStateStore
- **WHEN** the chunk blocks are loaded
- **THEN** SimulationCore queries `EntityStateStoreClient::LoadEntityState()` for the anchor position
- **AND** reconstructs the `MultiblockController` from the deserialized `MultiblockState`
- **AND** resumes multiblock tick

#### Scenario: Multiblock state deleted on dissociation
- **GIVEN** a multiblock is dissociated (anchor broken)
- **WHEN** dissociation cleanup runs
- **THEN** entity state for that mb_id is deleted from EntityStateStore

### Requirement: Spatial Queries
The system SHALL support spatial queries for multiblocks and entities.

#### Scenario: Find multiblocks in radius
- **GIVEN** a world position and radius
- **WHEN** SpatialIndex is queried
- **THEN** it returns all multiblocks with bounding boxes intersecting the query region

#### Scenario: Pattern matching uses direct ChunkStore queries
- **GIVEN** a controller block is placed at anchor position
- **WHEN** `PatternRegistry::matchAll()` runs
- **THEN** it queries `ChunkStore.getBlock()` for each pattern offset
- **AND** compares returned block_ids against pattern expectations

### Requirement: sim.multiblock Topics
The system SHALL publish multiblock lifecycle events (BREAKING).

#### Scenario: sim.multiblock.created published on EBF formation
- **GIVEN** an EBF multiblock is formed
- **WHEN** the `MultiblockController` is created
- **THEN** a `sim.multiblock.created` message is published via `IEventPublisher`
- **AND** the message payload contains `mb_id`, `controller_pos`, and `pattern_type`

#### Scenario: sim.multiblock.destroyed published on anchor break
- **GIVEN** a multiblock is dissociated
- **WHEN** the dissociation cascade completes
- **THEN** a `sim.multiblock.destroyed` message is published via `IEventPublisher`
- **AND** the message payload contains `mb_id`
