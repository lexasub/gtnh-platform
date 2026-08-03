## ADDED Requirements

### Requirement: Multiblock Pattern Library
The system SHALL support generic multiblock pattern matching.

#### Scenario: EBF pattern detected
- **GIVEN** a player builds a 3x3x4 hollow structure with casing blocks and two heating coil layers
- **WHEN** the controller block (id=1003) is placed at the anchor position
- **THEN** `onBlockChanged` triggers pattern matching via `PatternRegistry::matchAll()`
- **AND** SimulationCore detects the EBF pattern
- **AND** creates a `MultiblockController` ECS entity
- **AND** publishes `sim.multiblock.created`

#### Scenario: Large Boiler pattern detected
- **GIVEN** a player builds a 3x3x4 structure with a firebox bottom layer and casing walls
- **WHEN** the controller block (id=1005) is placed at the anchor position
- **THEN** SimulationCore detects the Large Boiler pattern
- **AND** creates a `MultiblockController` ECS entity

#### Scenario: LCR pattern detected
- **GIVEN** a player builds a 3x3x3 casing structure
- **WHEN** the controller block (id=1006) is placed
- **THEN** SimulationCore detects the LCR pattern
- **AND** creates a `MultiblockController` ECS entity

#### Scenario: Pattern mismatch on controller placement
- **GIVEN** a controller block is placed but the surrounding blocks do not match any registered pattern
- **WHEN** `PatternRegistry::matchAll()` runs
- **THEN** no `MultiblockController` is created
- **AND** the block exists as a standalone machine (managed_externally=false)

#### Scenario: Pattern matching reads blocks from ECS registry
- **GIVEN** a controller block is placed at anchor position
- **WHEN** `PatternRegistry::matchAll()` runs
- **THEN** it invokes the provided `BlockLookupFn` callback for each pattern offset
- **AND** the lookup reads block ids from the SimulationCore ECS registry (not ChunkStore)

#### Scenario: Pattern-relative hatch roles declared
- **GIVEN** a pattern is registered in `PatternLibrary`
- **THEN** hatch positions are declared pattern-relative with roles (FLUID_IN, FLUID_OUT, ENERGY, MUFFLER)
- **AND** the EBF pattern declares a MUFFLER hatch and an ENERGY hatch
- **AND** the Large Boiler pattern declares FLUID_IN and FLUID_OUT hatches
- **AND** the LCR pattern declares FLUID_IN, FLUID_OUT and ENERGY hatches

### Requirement: EBF Multiblock Tick
The system SHALL process EBF recipes respecting heating coil tier and heat requirements.

#### Scenario: EBF processes recipe with sufficient heat and energy
- **GIVEN** an EBF multiblock with TungstenSteel coils (max_heat=4500K)
- **AND** a recipe requiring 3000K is loaded
- **AND** the EnergyStorage component contains sufficient EU
- **WHEN** EBFSystem ticks
- **THEN** EU is consumed from EnergyStorage
- **AND** recipe progress advances
- **AND** output items appear in the container output slots
- **AND** a node update is published via `publishNodeUpdate`

#### Scenario: EBF pauses when heat insufficient
- **GIVEN** an EBF multiblock with Kanhal coils (max_heat=1800K)
- **AND** a recipe requiring 3000K is loaded
- **WHEN** EBFSystem ticks
- **THEN** recipe progress is paused
- **AND** no EU is consumed

#### Scenario: EBF requests energy when energy insufficient
- **GIVEN** an EBF with an active recipe
- **AND** EnergyStorage.current below the recipe's energy cost
- **WHEN** EBFSystem ticks
- **THEN** a consume request is sent via `PipeEnergyClient::sendConsumeRequest`
- **AND** recipe progress is paused

#### Scenario: EBF heating coil determines max heat
- **GIVEN** an EBF with Kanhal coils (block_id=KANHAL_COIL)
- **WHEN** EBFSystem reads the coil layers
- **THEN** max_heat = 1800K
- **WHEN** coils are Nichrome (block_id=NICHROME_COIL)
- **THEN** max_heat = 2700K
- **WHEN** coils are TungstenSteel (block_id=TUNGSTENSTEEL_COIL)
- **THEN** max_heat = 4500K
- **AND** both coil layers are read, taking the minimum of the two

#### Scenario: EBF starts recipe from container inputs
- **GIVEN** an EBF with no active recipe
- **AND** input items in the container input slots matching a registered EBF recipe
- **WHEN** EBFSystem ticks
- **THEN** `RecipeManager::findRecipeByInputs()` returns the recipe
- **AND** inputs are consumed from the container
- **AND** recipe progress starts

### Requirement: Large Boiler Multiblock Tick
The system SHALL process large boiler operation: fuel burning and steam production.

#### Scenario: Large Boiler produces steam
- **GIVEN** a Large Boiler multiblock with coal in a container slot
- **WHEN** LargeBoilerSystem ticks
- **THEN** fuel count decrements
- **AND** heat is added to HeatIntakeComponent (capped at capacity)
- **AND** steam is produced via `EnergyStorage::addEnergy(STEAM_PER_WATER)`
- **AND** a node update with STEAM energy type is published to PipeNetwork via `publishNodeUpdate`

#### Scenario: Large Boiler cools without fuel
- **GIVEN** a Large Boiler with no fuel in container slots
- **AND** heat stored above zero
- **WHEN** LargeBoilerSystem ticks
- **THEN** heat decreases by 5 per tick

#### Scenario: Large Boiler overheats without fuel
- **GIVEN** a Large Boiler with no fuel in container slots
- **AND** heat stored above 80% of capacity
- **WHEN** LargeBoilerSystem ticks
- **THEN** OverheatComponent is set to WARNING

### Requirement: LCR Multiblock Tick
The system SHALL process LCR recipes with item inputs.

#### Scenario: LCR processes item-input recipe
- **GIVEN** an LCR multiblock with input items in the container matching a registered recipe
- **AND** sufficient EU in EnergyStorage
- **WHEN** LCRSystem ticks
- **THEN** `RecipeManager::findRecipeByInputs()` returns the recipe
- **AND** inputs are consumed from the container
- **AND** EU/tick is consumed from EnergyStorage
- **AND** outputs are produced to the container output slots

#### Scenario: LCR pauses without energy
- **GIVEN** an LCR with an active recipe
- **AND** insufficient EU in EnergyStorage
- **WHEN** LCRSystem ticks
- **THEN** recipe progress is paused
- **AND** a consume request is sent via `PipeEnergyClient::sendConsumeRequest`

### Requirement: Multiblock Dissociation
The system SHALL detect and handle multiblock destruction.

#### Scenario: Anchor block broken disassembles multiblock
- **GIVEN** a multiblock exists with a controller at the anchor position
- **WHEN** the anchor block is broken (becomes air)
- **THEN** SimulationCore detects the controller entity at that position in `onBlockChanged()`
- **AND** serializes the final multiblock state via `serializeMultiblock()`
- **AND** invokes `onMultiblockSave` to persist the state
- **AND** destroys the `MultiblockController`
- **AND** publishes `sim.multiblock.destroyed`

#### Scenario: Non-anchor block removal removes block from controller
- **GIVEN** a multiblock exists with a casing piece
- **WHEN** the casing piece is broken (becomes air)
- **THEN** `removeBlockFromController(mb_id, pos)` removes the packed position from `MultiblockController.blocks`
- **AND** the multiblock continues to exist (controller intact)

### Requirement: Multiblock Persistence
The system SHALL persist and restore multiblock state via EntityStateStore.

#### Scenario: Multiblock state saved on dissociation
- **GIVEN** a multiblock with an active controller
- **WHEN** the anchor block is broken
- **THEN** SimulationCore serializes `MultiblockState` (controller_id, anchor, pattern_id, blocks, heat_stored, recipe_id, recipe_progress)
- **AND** calls `EntityStateStoreClient::SaveEntityState()` with entity_type=4 (MULTIBLOCK)
- **AND** the blob is stored under the anchor position

#### Scenario: Multiblock state restored on controller creation
- **GIVEN** a multiblock is formed at a position with previously saved state in EntityStateStore
- **WHEN** the `MultiblockController` is created
- **THEN** SimulationCore queries `EntityStateStoreClient::LoadEntityState()` for the anchor position with entity_type=4
- **AND** reconstructs heat and recipe progress via `deserializeMultiblock()`
- **AND** resumes multiblock tick

### Requirement: sim.multiblock Topics
The system SHALL publish multiblock lifecycle events.

#### Scenario: sim.multiblock.created published on EBF formation
- **GIVEN** an EBF multiblock is formed
- **WHEN** the `MultiblockController` is created
- **THEN** a `sim.multiblock.created` message is published via `IEventPublisher`
- **AND** the message payload contains `controller_id`, anchor position, and pattern id

#### Scenario: sim.multiblock.destroyed published on anchor break
- **GIVEN** a multiblock is dissociated
- **WHEN** the dissociation cascade completes
- **THEN** a `sim.multiblock.destroyed` message is published via `IEventPublisher`
- **AND** the message payload contains `controller_id`

#### Scenario: Gateway forwards multiblock events to client
- **GIVEN** a `sim.multiblock.created` or `sim.multiblock.destroyed` message arrives at Gateway
- **WHEN** Gateway matches the subscription
- **THEN** it forwards the event to the client's control connection as `kMultiblockEvent` (GatewayMsg id 23)

#### Scenario: Client receives multiblock events
- **GIVEN** the game client is connected
- **WHEN** a multiblock event arrives on the control connection
- **THEN** NetClient handles `kMultiblockEvent` and logs the lifecycle event
