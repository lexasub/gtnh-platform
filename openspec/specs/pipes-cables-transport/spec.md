# pipes-cables-transport Specification

## Purpose
TBD - created by archiving change implement-pipes-cables-transport. Update Purpose after archive.
## Requirements
### Requirement: Item Pipe Transport
The system SHALL support item transport between machines via item pipes, with BFS routing and 1 block/tick movement.

**References:**
- `src/services/pipe_network/PipeNetworkManager.cpp` — `addNode()`, `rebuildItemNetworks()`, `findNextItemHop()`
- `src/services/pipe_network/PipeNetworkService.cpp` — `handleBlockChanged()`, `handleItemNodeUpdate()`, `handleItemTransferRequest()`
- `src/services/pipe_network/PipeBlockIds.h` — `BLOCK_ID_ITEM_PIPE`, `BLOCK_ID_DENSE_ITEM_PIPE`
- `src/protocol/pipe_network.fbs` — `ItemNodeUpdate`, `ItemTransferReq/Resp`, `ItemFlowEvent`

#### Scenario: Item pipe block placed
- **GIVEN** no item pipe network exists
- **WHEN** a player places `item_pipe` (block_id=62) adjacent to an existing pipe or machine
- **THEN** `PipeNetworkService::handleBlockChanged()` fires
- **AND** `PipeNetworkManager::addNode()` creates a new pipe node
- **AND** `rebuildItemNetworks()` rebuilds connected components via BFS

#### Scenario: Item pipe block broken
- **GIVEN** an item pipe is connected to a network
- **WHEN** the player breaks the pipe block
- **THEN** `PipeNetworkManager::removeNode()` removes the node
- **AND** `rebuildItemNetworks()` splits the network into separate components

#### Scenario: Item moves from machine output to pipe
- **GIVEN** a machine with `OUTPUT` role on a face connected to an item pipe
- **WHEN** the machine finishes a recipe
- **THEN** the output item is pushed into the connected pipe
- **AND** `ItemNodeUpdate` is published on topic `item.node.update`

#### Scenario: Item moves 1 block/tick along pipe network
- **GIVEN** an item pipe network connects two machines
- **WHEN** an item enters the network
- **THEN** `PipeNetworkManager::findNextItemHop()` computes the next node on the path
- **AND** the item advances 1 block per tick toward the destination

#### Scenario: Item reaches machine input slot
- **GIVEN** a pipe network delivers an item to a machine with `INPUT` role
- **WHEN** the item reaches the machine's connected pipe face
- **THEN** the item is inserted into the machine's input inventory slot
- **AND** `SetMachineSlotReq` is sent to SimulationCore

#### Scenario: Dense item pipe has higher capacity
- **GIVEN** a `dense_item_pipe` (block_id=64) and a regular `item_pipe` (block_id=62)
- **WHEN** both are placed in the network
- **THEN** the dense pipe node has `itemCapacity > item_pipe` capacity
- **AND** more items can be buffered at the dense pipe node

### Requirement: Fluid Pipe Transport

The system SHALL support fluid transport between machines via fluid pipes, with
fluid type tracking, per-fluid connected components, and volume distribution.
Each machine resource port SHALL be registered independently, so a converter may
be both a source and a sink in different resource domains. Resource channels are
`FLUID`, `EU`, `HU`, `RU`, and `ITEM`; Steam is a `FLUID`, not a separate channel.
Ports have no resource filters in this change. The concrete resource identity for
FLUID and ITEM is always the canonical packed ID from `items.csv`. A fluid pipe
network MUST never combine different fluid item IDs, while an ITEM output port
may emit different concrete item IDs over time.

#### Scenario: Fluid pipe block placed
- **GIVEN** no fluid pipe network exists
- **WHEN** a player places a registered `fluid_pipe` adjacent to an existing pipe
  or compatible fluid port
- **THEN** the network manager creates a fluid node using the canonical item ID
  from `items.csv`
- **AND** the node has its configured capacity and an empty fluid buffer

#### Scenario: Fluid flows through pipe network
- **GIVEN** a fluid pipe network connects a source port and a compatible sink port
- **WHEN** the source owner accepts a `DrainRequest`
- **THEN** exactly the accepted amount enters PipeNetwork-owned pipe buffers
- **AND** distribution is limited by the source rate, pipe capacity, sink demand,
  and canonical `fluid_id`
- **AND** any flow event is telemetry and cannot mutate an owner buffer

#### Scenario: Fluid inserted into destination machine
- **GIVEN** a fluid pipe network has fluid available for a compatible sink port
- **WHEN** the sink sends a `ConsumeRequest`
- **THEN** PipeNetwork drains its pipe buffers first
- **AND** requests any shortfall from compatible source owners
- **AND** `ConsumeResponse.accepted_amount` reports the exact amount accepted by
  the sink
- **AND** the sink machine buffer increases only once from that response

#### Scenario: Fluid type mismatch blocks flow
- **GIVEN** a fluid pipe network contains `fluid_id = 1` (water)
- **WHEN** a source or sink with `fluid_id = 2` (steam) requests transport on the
  same network
- **THEN** the mismatched amount is not accepted or mixed
- **AND** the caller receives a blocked or short-fill response
- **AND** the existing fluid buffer remains unchanged

#### Scenario: Fluid requests are replay-safe
- **GIVEN** a source drain or sink consume request with `request_id = R`
- **WHEN** the same request is delivered more than once
- **THEN** the owner returns the cached response for `R`
- **AND** the resource is debited or credited only once

#### Scenario: Fluid port removal
- **GIVEN** a machine has one or more registered resource ports
- **WHEN** the machine is removed or its block is replaced
- **THEN** all ports owned by that machine are unregistered
- **AND** their pending requests are completed as zero/short-fill
- **AND** no future flow targets the removed machine

### Requirement: Energy Cable Transport
The system SHALL support packet-based energy transport through cables with voltage tier enforcement, loss calculation, and overheat/explosion mechanics.

**References:**
- `src/services/pipe_network/CableGraph.h/.cpp` — `addCableNode()`, `injectPacket()`, `collectPackets()`, `tick()`, `processOverheat()`
- `src/services/pipe_network/CableTypes.h` — `CableDef`, `CABLE_DEFS` map, `isCableBlock()`, `getCableDef()`
- `src/services/pipe_network/CableLoss.h` — `effectiveVoltage()` for voltage drop
- `src/services/pipe_network/PipeNetworkService.cpp` — cable node wiring in `handleBlockChanged()`
- `src/protocol/pipe_network.fbs` — `EnergyNodeUpdate`, `EnergyPacketDef`, `CableNodeStatus`, `CableExplodedEvent`

#### Scenario: Cable block placed
- **GIVEN** no cable network exists
- **WHEN** a player places `cable_tin` (block_id=66) adjacent to another cable or machine
- **THEN** `CableGraph::addCableNode()` creates a new cable node with tier/ampacity/loss from `CABLE_DEFS`

#### Scenario: Correct tier allows energy flow
- **GIVEN** a cable of tier T connects a generator to a machine
- **WHEN** the generator outputs voltage ≤ cable's `maxVoltage`
- **THEN** energy flows normally through the network
- **AND** `CableGraph::injectPacket()` routes the energy packet

#### Scenario: Cable loss per block reduces voltage
- **GIVEN** a cable of type `cable_tin` with `loss_per_block = 0x05f5e100`
- **WHEN** an energy packet travels `N` blocks through the cable
- **THEN** the effective voltage at destination = `voltage - (loss_per_block * N)`
- **AND** `CableLoss::effectiveVoltage()` computes the reduced voltage

#### Scenario: Overvoltage causes cable overheat
- **GIVEN** a cable of tier T with `maxVoltage = V`
- **WHEN** incoming packets exceed `V` (tracked via `maxSeenVoltage`)
- **THEN** the cable's `temperature` rises each tick
- **AND** `packetsThisTick > ampacity` also contributes to temperature increase

#### Scenario: Critical overheat causes cable explosion
- **GIVEN** a cable node's `temperature` reaches critical threshold (`1.0f`)
- **WHEN** `CableGraph::processOverheat()` runs during `tick()`
- **THEN** the cable node is removed from the graph
- **AND** added to `getExplodedNodes()` for the current tick
- **AND** `CableExplodedEvent` is published with position and temperature

### Requirement: Transformers
The system SHALL support voltage transformation between cable tiers, both step-up (low→high voltage) and step-down (high→low voltage).

**References:**
- `src/services/simulation_core/ECS/Systems/TransformerSystem.cpp` — `tick()`, `isTransformer()`, step-up/down logic
- `src/services/simulation_core/ECS/Systems/TransformerSystem.h` — class declaration
- Block IDs: `1110:11:0` (transformer_mv_hv), `1110:11:1` (transformer_hv_ev)

#### Scenario: Transformer step-up converts voltage
- **GIVEN** a `transformer_mv_hv` at tier 2 (MV, 128V) input / tier 3 (HV, 512V) output
- **WHEN** multiple MV packets arrive at the input buffer
- **THEN** `TransformerSystem::tick()` accumulates low-voltage energy
- **AND** when buffer ≥ output voltage, emits high-voltage packets
- **AND** the voltage ratio determines packet conversion: 4 MV packets → 1 HV packet

#### Scenario: Transformer step-down distributes voltage
- **GIVEN** a `transformer_hv_ev` at tier 3 (HV, 512V) input / tier 4 (EV, 2048V) output
- **WHEN** a step-down mode is active (`tf.stepUp = false`)
- **THEN** high-voltage energy is received
- **AND** distributed to `EnergyStorage` for low-voltage consumption

#### Scenario: Transformer block placed
- **GIVEN** a transformer is placed between two cable tiers
- **WHEN** the block is registered in SimulationCore ECS
- **THEN** `TransformerComponent` and `EnergyStorage` components are attached
- **AND** `EnergyNodeUpdate` is published to register with PipeNetworkService

### Requirement: Pipe/Cable Block Registration
The system SHALL register all pipe and cable block IDs in the item registry and provide classification functions for rendering and simulation.

**References:**
- `data/registry/items.csv` — block ID entries (lines 106-116)
- `src/services/pipe_network/PipeBlockIds.h` — `BLOCK_ID_FLUID_PIPE`, `BLOCK_ID_ITEM_PIPE`, `BLOCK_ID_DENSE_ITEM_PIPE`, `BLOCK_ID_DENSE_FLUID_PIPE`
- `src/services/pipe_network/PipeNetworkService.cpp` — `isPipeBlock()` at line 276
- `src/services/pipe_network/CableTypes.h` — `isCableBlock()` at line 27
- `src/services/game_client/Render/BlockRenderRegistry.h` — `isPipeBlock()`, `isCableBlock()`, `blockIdToPipeType()`, `blockIdToCableTier()`
- `src/services/game_client/Render/PipeMeshBuilder.h` — `pipeTypeToBlockId()`, `isCableType()`, `pipeTypeToCableTier()`

#### Scenario: Pipe block IDs registered
- **GIVEN** the item registry at `data/registry/items.csv`
- **WHEN** scanning registered items
- **THEN** `fluid_pipe` (61), `item_pipe` (62), `dense_item_pipe` (64), `dense_fluid_pipe` (65) are present

#### Scenario: Cable block IDs registered
- **GIVEN** the item registry at `data/registry/items.csv`
- **WHEN** scanning registered items
- **THEN** `cable_tin` (66) through `cable_platinum` (71) are present

#### Scenario: isPipeBlock() returns true for pipe blocks
- **GIVEN** `PipeNetworkService::isPipeBlock()`
- **WHEN** called with block_id = 61 (fluid_pipe), 62 (item_pipe), 64 (dense_item_pipe), 65 (dense_fluid_pipe)
- **THEN** returns `true`
- **AND** called with block_id = 0, 66 (cable_tin), or any non-pipe block
- **THEN** returns `false`

#### Scenario: isCableBlock() returns true for cable blocks
- **GIVEN** `isCableBlock()` from `CableTypes.h`
- **WHEN** called with block_id = 66..71 (cable_tin through cable_platinum)
- **THEN** returns `true`
- **AND** called with block_id = 62 (item_pipe) or non-cable block
- **THEN** returns `false`

### Requirement: Cable Overheat Explosion Propagation
The system SHALL propagate cable explosion events back to the world by replacing exploded cable blocks with air.

**References:**
- `src/services/pipe_network/CableGraph.h` — `getExplodedNodes()` at line 58
- `src/services/pipe_network/CableGraph.cpp` — `processOverheat()` at line 252
- `src/protocol/pipe_network.fbs` — `CableExplodedEvent` at line 116

#### Scenario: Exploded cable converted to air block
- **GIVEN** a cable node has exploded during `CableGraph::tick()`
- **WHEN** `getExplodedNodes()` returns the exploded node
- **THEN** a `SetBlock` request is sent to ChunkStore for that position with block_id=0 (air)
- **AND** the explosion event is logged and optionally creates particle effects

### Requirement: Item Buffer Persistence
The system SHALL persist items in transit through pipe networks to EntityStateStore for crash recovery.

**References:**
- `src/services/entity_state_store/` — LMDB-backed entity state store
- `src/services/pipe_network/PipeNetworkManager.cpp` — `itemBuffer` per node

#### Scenario: In-transit items saved periodically
- **GIVEN** items are moving through a pipe network
- **WHEN** the server saves state (interval-based or on chunk unload)
- **THEN** each pipe node's `itemBuffer` is serialized and sent to EntityStateStore
- **AND** on server restart, pipe item buffers are restored from persistence

