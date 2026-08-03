# electric-tools-wrench Specification

## Purpose

Define requirements for electric tool energy management, wrench-based machine face cycling with side_config, and the client-server interaction flow including cooldown and texture updates.
## Requirements
### Requirement: Client Raycast Face Detection (G key)
The client SHALL detect the targeted block face via raycast when the player presses G while holding a wrench, and send a ToolAction frame to the server.

NOTE: Core implementation (G key binding + raycast + face detection + SendToolAction + itemId + wrench check + ToolActionResp handler) exists in `InteractionSystem.cpp:76-98` and `GameClient.cpp:102-117`.

#### Scenario: G key with wrench sends ToolAction
- **GIVEN** the player holds a wrench tool and looks at a machine face
- **WHEN** the player presses G
- **THEN** `InteractionSystem::Update()` (`src/services/game_client/World/InteractionSystem.cpp:48`) performs `Raycaster::GetTargetedBlock()` (`src/services/game_client/RenderLib/Utils/Raycaster.h:17`)
- **AND** converts the face normal → face index (DOWN=0, UP=1, NORTH=2, SOUTH=3, WEST=4, EAST=5)
- **AND** calls `NetClient::SendToolAction(playerId, WRENCH_CYCLE, x, y, z, faceIdx, itemId)`
- **AND** the frame includes the held item's `itemId` so the server can validate the tool type

#### Scenario: Non-wrench item in hand ignored
- **GIVEN** the player holds a non-wrench item (dirt, stone, drill)
- **WHEN** pressing G
- **THEN** no ToolAction is sent
- **AND** the check is done server-side: `WrenchHandler::cycleFace()` validates held item type

#### Scenario: Non-machine block handled server-side
- **GIVEN** the player looks at a non-machine block (dirt, stone, air)
- **WHEN** pressing G
- **THEN** `InteractionSystem` sends the ToolAction (client does not filter by block type)
- **AND** the server responds with `ToolActionResp(success=false, error="not_a_machine")`
- **AND** the client shows a failure notification

#### Scenario: Server-side cooldown prevents spam
- **GIVEN** the player holds G continuously
- **WHEN** `InteractionSystem` sends WRENCH_CYCLE each frame (no edge detection in InputState)
- **THEN** the server SHALL deduplicate requests per `playerId + pos + face` with ~200ms cooldown
- **AND** `WrenchActionHandler` SHALL track last action tick in a cooldown map

#### Scenario: Out of reach ignored
- **GIVEN** the machine is beyond `Raycaster::REACH_DIST` (5.0 blocks)
- **WHEN** pressing G
- **THEN** no ToolAction is sent (raycast returns no hit)

---

### Requirement: Client Machine Texture on Side Config Change
The client SHALL update machine face textures when `world.machine.config.updated` is received from the server.

#### Scenario: Texture update on server event
- **GIVEN** the server publishes `Protocol::MachineConfigUpdated` on `"world.machine.config.updated"` topic
- **WHEN** the client receives the event at position (x,y,z) with `side_config[6]`
- **THEN** the client updates face textures for each face to match the new role
- **AND** triggers a mesh rebuild at that block position

#### Scenario: Connect/disconnect state
- **GIVEN** the machine face role was set to INPUT
- **THEN** the client shows an INPUT arrow or color overlay on that face
- **GIVEN** the machine face role was set to NONE
- **THEN** the client shows the default face texture (no pipe/cable hole)

#### Scenario: ToolActionResp confirms authority
- **GIVEN** the server processes a WRENCH_CYCLE action
- **WHEN** `ToolActionResp(success=true)` is received on client
- **THEN** the client waits for `world.machine.config.updated` event before updating textures
- **AND** does NOT optimistically update textures on send

---

### Requirement: PipeNetwork BFS Side Config Filtering
PipeNetwork SHALL respect `MachineComponent::side_config` roles during BFS traversal and machine registration.

#### Scenario: ENERGY-only face routing
- **GIVEN** a machine's NORTH face has side_config role = ENERGY
- **WHEN** PipeNetwork BFS (`CableGraph::rebuildGraph()`, `CableGraph.cpp:43-93`) discovers connections
- **THEN** only cables connected to the NORTH face participate in energy routing
- **AND** cables connected to NONE/INPUT/OUTPUT faces on the same machine are skipped for energy

#### Scenario: PipeNetwork receives side_config via event subscription
- **GIVEN** `WrenchHandler::cycleFace()` publishes `MachineConfigUpdated` on `"world.machine.config.updated"`
- **WHEN** PipeNetwork receives the event via subscription
- **THEN** `PipeNetworkService` caches `(x,y,z) → side_config[6]` in a local map
- **AND** `CableGraph` uses the cached config for BFS adjacency filtering

#### Scenario: Machine registration with side_config
- **GIVEN** a machine is registered via `CableGraph::registerMachine(entityId, x, y, z, sideConfig[6])`
- **WHEN** `side_config[6]` is provided
- **THEN** only faces with role ≠ NONE and matching transport type (ENERGY for cables) are used
- **AND** face roles with mismatched types (e.g., FLUID_IN on a cable connection) are treated as NONE

---

### Requirement: Item EnergyStorage for Tools
Tools SHALL have EnergyStorage independent of machine EnergyStorage, tracked per ItemStack via `meta` field.

NOTE: `ItemEnergyStorage.h` already exists with `TOOL_ENERGY_DEFS`, `getToolEnergy()`, `setToolEnergy()`, `consumeToolEnergy()`. Battery buffer charging already works. Gap: `DrillSystem` uses machine-level `EnergyStorage` instead of item-level.

#### Scenario: Drill consumes from item energy, not machine energy
- **GIVEN** a drill tool with energy > 0 (tracked in `ItemStack.meta`)
- **WHEN** `DrillSystem` mines a block (`src/services/simulation_core/ECS/Systems/DrillSystem.cpp`)
- **THEN** `phaseEnergyCheck()` calls `consumeToolEnergy(drillItem, drill_energy_per_block)` instead of `energy.consumeEnergy()`
- **AND** if energy reaches 0, drill stops mining
- **AND** returns `ToolActionResp(success=false, reason="out_of_energy")`

#### Scenario: Battery buffer recharges tool
- **GIVEN** a battery buffer has stored energy and PipeNetwork connection
- **WHEN** a tool with energy < capacity is placed in its slot
- **THEN** `BatteryBufferSystem::chargeSlot()` (`BatteryBufferSystem.cpp:76-101`) transfers up to `chargeRate` EU/tick
- **AND** `TOOL_ENERGY_DEFS` defines per-tool `capacity`, `maxInput`, `tier`
- **AND** energy is stored in `ItemStack.meta` via `setToolEnergy()`

#### Scenario: Client shows out-of-energy warning
- **GIVEN** a drill with energy reaches 0 while mining
- **WHEN** the server publishes `ToolActionResp(success=false, reason="out_of_energy")`
- **THEN** the client shows a warning toast "Tool out of energy"

