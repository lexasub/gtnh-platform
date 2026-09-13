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

### Requirement: Wrench on Pipe Targets
When a `WRENCH_CYCLE` ToolAction targets a pipe block, SimulationCore SHALL route it through a dedicated pipe-wrench flow instead of the machine side-config flow, and SHALL return actionable connection guidance to the player.

#### Scenario: Wrenching an isolated pipe
- GIVEN a player wrenches a pipe block with no adjacent pipe or machine
- WHEN SimulationCore processes the `WRENCH_CYCLE` ToolAction for that position
- THEN SimulationCore SHALL classify the target block server-authoritatively (ECS machine entity first, then a ChunkStore block query)
- AND SHALL publish a `PipeWrenchAction { player_id, pos, face }` on topic `pipe.wrench.action`
- AND upon `PipeWrenchResp` guidance `CONNECT_PIPES` SHALL publish a `ToolActionResp` with a guidance message to place adjacent pipes
- AND SHALL NOT cycle any machine `side_config`

#### Scenario: Wrenching a pipe in a pipe-only network
- GIVEN a pipe block that is part of a pipe network with no machine connection
- WHEN the player wrenches it
- THEN upon guidance `CONNECT_TO_MACHINE` SimulationCore SHALL publish a `ToolActionResp` telling the player to connect the pipe to a machine

#### Scenario: Wrenching a connected pipe
- GIVEN a pipe block that is connected to a machine network
- WHEN the player wrenches it
- THEN upon guidance `CONNECTED` SimulationCore SHALL publish a `ToolActionResp` confirming the pipe is connected
- AND the guidance message SHALL include the connected network's node count (from `PipeWrenchResp.node_id` + component size) when available

#### Scenario: Server-authoritative target classification
- GIVEN a player wrenches any block position
- WHEN SimulationCore processes the action
- THEN an existing ECS machine entity at the position SHALL take precedence and use the machine side-config flow
- AND otherwise the target block SHALL be queried from ChunkStore, not taken from client-supplied data
- AND pipe blocks SHALL NOT be rejected with the generic `not_a_machine` error

#### Scenario: Non-machine, non-pipe targets still rejected
- GIVEN the player wrenches a block that is neither a machine nor a pipe
- WHEN SimulationCore classifies the target
- THEN it SHALL respond with `ToolActionResp(success=false)` as before

### Requirement: Client Wrench Guidance Toast
The client SHALL display the `message` field of a received `ToolActionResp` as a transient toast notification.

#### Scenario: Guidance message displayed
- GIVEN the server publishes a `ToolActionResp` with a non-empty `message`
- WHEN the client receives `kToolActionResp` (14)
- THEN the message SHALL be shown to the player as a toast overlay
- AND the toast SHALL auto-dismiss after a short lifetime

#### Scenario: Empty message shows nothing
- GIVEN a `ToolActionResp` with an empty `message`
- WHEN the client receives it
- THEN no toast SHALL be shown

### Requirement: GT-Style Wrench Overlay
When the player holds a wrench and targets a wrenchable block (machine or pipe), the client SHALL render a GregTech-style wrench overlay on the targeted block in addition to the highlight: an in-world markup (not a UI window) of corner crosses and direction bars on the block edges. The overlay SHALL act as a face selector — clicking a direction bar SHALL send the wrench action targeting that face, so the player can connect a pipe to a specific side by clicking that side's bar.

#### Scenario: Overlay rendered while holding wrench
- GIVEN the player holds a wrench and the raycast targets a machine or pipe block
- WHEN the frame renders
- THEN the client SHALL draw a screen-space silhouette of the block with four direction bars along its edges (top/bottom/left/right)
- AND SHALL draw crosses at the four corners where the bars intersect
- AND the overlay SHALL be rendered on the UI/HUD (not silently skipped)
- AND the bar matching the current raycast face SHALL be visually highlighted as preselected

#### Scenario: No overlay without wrench
- GIVEN the player does not hold a wrench
- WHEN the raycast targets any block
- THEN the client SHALL render only the plain highlight wireframe

#### Scenario: Clicking a direction bar selects that face
- GIVEN the player holds a wrench and a wrenchable block is targeted with the overlay visible
- WHEN the player clicks on the bar for the right side
- THEN the client SHALL send a wrench action (`WRENCH_CYCLE`) with the right-side face, regardless of where the raycast hit the block
- AND for a pipe target the connection evaluation SHALL be performed for that side

#### Scenario: Clicking a corner cross selects the front/back depth face
- GIVEN the overlay is visible on a wrenchable block with the four corner crosses drawn
- WHEN the player clicks a corner cross (where two edge bars intersect)
- THEN the client SHALL send the wrench action targeting the depth-axis face toward or away from the player (front/back), not a screen-edge face
- AND the cross SHALL be visually distinct from the edge bars to signal the perpendicular (connect-backward) direction
- AND for a pipe target the connection evaluation SHALL be performed on that depth face

#### Scenario: Raycast face remains the default
- GIVEN the overlay is visible and the player does not click a bar
- WHEN the player triggers the wrench action (G key)
- THEN the action SHALL target the current raycast face, as today

#### Scenario: Pipe connectable directions shown
- GIVEN the player holds a wrench and the raycast targets a pipe block
- WHEN an adjacent block in a face direction is itself a pipe or a machine
- THEN the direction bar for that face SHALL be rendered as connectable
- AND directions with no adjacent pipe or machine SHALL be dimmed or omitted
- AND the direction data SHALL be derived from client-local world state; the server response remains authoritative for actual connection guidance

#### Scenario: Overlay hidden when block not targeted
- GIVEN the player holds a wrench but no block is targeted
- WHEN the frame renders
- THEN no wrench overlay SHALL be drawn

