## Purpose

Define the system architecture of GTNH Platform — a distributed Minecraft-style engine with a C++ performance core and Go sidecars. Services communicate over TCP via a unified binary protocol (FlatBuffers + Asio) through a MessageRouter pub/sub layer.
## Requirements
### Requirement: System Topology
The GTNH Platform SHALL consist of 9+ microservices communicating over TCP via a unified binary protocol.

#### Scenario: Services run as separate processes
- **GIVEN** the platform is deployed
- **THEN** each service runs as an independent OS process
- **AND** they communicate over TCP (direct or via MessageRouter)

#### Scenario: Startup order enforced
- **GIVEN** the platform starts
- **THEN** MessageRouter starts first (port 4000)
- **AND** ChunkStore starts second (port 5001)
- **AND** remaining services start after
- **AND** the client starts last

### Requirement: Service Inventory
The system SHALL define the following services with their responsibilities and communication patterns.

| Service | Language | Port(s) | Responsibility | Communication |
|---------|----------|---------|---------------|---------------|
| MessageRouter | Go | 4000 | Internal pub/sub, heartbeat, service discovery | TCP (all services) |
| Gateway | C++ | 7777 (ctrl), 7778 (bulk), 4000 (router) | TCP gateway, interest management, client relay | TCP client + router |
| ChunkStore | C++ | 5001 (RPC), 4000 (router) | Block data, LMDB persistence, chunk load/save | Router pub/sub + RPC |
| WorldGenerator | C++ | (library) | Chunk generation, procedural terrain, ore distribution | Linked into ChunkStore |
| SimulationCore | C++ | 4000 (router, нет собственного RPC-порта) | ECS (EnTT), machine 20Hz tick, multiblock detection, crafting | Router pub/sub + RPC |
| PipeNetwork | C++ | 4000 (router) | Energy/liquid/item flow graphs, BFS solving | Router pub/sub |
| SpatialIndex | C++ | — | R-tree/Octree, multiblock queries | Deferred (L2) |
| MetaDB | Go | 5005 (RPC), 4000 (router) | Player saves, inventories, SQLite persistence | Router pub/sub + RPC |
| EntityStateStore | C++ | 5200 (RPC), 4000 (router) | World-bound entity state (tile entities), LMDB | Router pub/sub + RPC |
| GameClient | C++ | 7777 (gw ctrl), 7778 (gw bulk) | bgfx render, ImGui, input, world mesh | TCP to Gateway |

#### Scenario: Gateway accepts client connections
- **GIVEN** the Gateway is running on port 7777 (ctrl) / 7778 (bulk)
- **WHEN** a GameClient connects via TCP
- **THEN** Gateway creates a session
- **AND** forwards relevant messages between client and internal services

#### Scenario: SimulationCore ticks machines at 20Hz
- **GIVEN** SimulationCore is running
- **WHEN** the ECS engine ticks
- **THEN** MachineSystem iterates all machines with `needsTick=true`
- **AND** processes recipes, energy consumption, and progress

### Requirement: Data Ownership
The system SHALL enforce clear data ownership boundaries between services.

#### Scenario: ChunkStore owns block data
- **GIVEN** a block exists in the world
- **THEN** ChunkStore is the single source of truth for `block_id`, `meta`, and `mb_id`
- **AND** no other service persists block data

#### Scenario: SimulationCore owns multiblocks
- **GIVEN** a multiblock structure exists
- **THEN** SimulationCore owns the `MultiblockController` ECS entity
- **AND** ChunkStore stores only the `mb_id` reference in chunk meta-layer

#### Scenario: Chunk unload requires coordination
- **GIVEN** ChunkStore wants to unload a chunk
- **WHEN** the chunk contains multiblock references
- **THEN** ChunkStore asks SimulationCore for release
- **AND** SimulationCore releases only if the multiblock anchor is inside the chunk
- **AND** ChunkStore unloads only after all `mb_id` values are released

### Requirement: Event-Driven Communication
The system SHALL use event-driven patterns for cross-service communication.

#### Scenario: Block change propagates through router
- **GIVEN** a player places a block
- **WHEN** SimulationCore confirms the placement via ChunkStore RPC
- **THEN** ChunkStore publishes `world.blocks.changed` on MessageRouter
- **AND** SimulationCore and Gateway receive the event
- **AND** Gateway forwards `BlockChangedEvent` to the client

#### Scenario: PipeNetwork solves flow graphs
- **GIVEN** PipeNetwork receives energy node updates
- **WHEN** the flow graph changes
- **THEN** PipeNetwork runs BFS to solve energy/liquid distribution
- **AND** publishes flow results on MessageRouter
- **AND** caches results if network unchanged for 5 seconds

### Requirement: Protocol Constraints
The system SHALL adhere to strict protocol and language constraints.

#### Scenario: Gateway uses zero-copy binary
- **GIVEN** Gateway processes client messages
- **THEN** it MUST parse FlatBuffers in-place from TCP receive buffers
- **AND** MUST NOT use JSON for any client-facing protocol

#### Scenario: Hot path is C++ only
- **GIVEN** a service is on the hot path (ChunkStore, SimulationCore, Gateway)
- **THEN** it MUST be implemented in C++
- **AND** Go SHALL NOT be used due to GC latency

### Requirement: Visual Architecture
The system SHALL maintain C4 model diagrams for architecture visualization.

#### Scenario: C4 diagrams exist for all levels
- **GIVEN** the project documentation
- **THEN** `doc/c4/` SHALL contain PlantUML files for Levels 1–4
- **AND** Level 1 SHALL show system context
- **AND** Level 2 SHALL show container topology (all services + connections)
- **AND** Level 3 SHALL show component internals per service
- **AND** Level 4 SHALL show detailed class/data flow diagrams

### Requirement: Canonical Cross-Registry Item Identity

`data/registry/items.csv` SHALL be the sole canonical catalog and ID namespace
for every item, block, pipe, cable, and fluid. Every specialized registry row
is keyed by an `item_id` that MUST resolve to exactly one row in `items.csv`:
`pipes.csv.item_id`, `cables.csv.item_id`, and `fluids.csv.item_id`; a
`drops.csv` source and result also resolve there. Specialized CSVs contain only
properties and MUST NOT define a second numeric ID for the same object.

There is no separate `fluid_id`, `pipe_id`, or `cable_id`. Steam is the packed
`ItemId::pack("1111:11:1")` from `items.csv`; transport uses that same value in
fluid operations, while `ResourceKind::FLUID` identifies the channel. Pipe and
cable nodes likewise use their packed `items.csv` ID. Code MUST NOT substitute a
row number, array index, or legacy small integer for a canonical item ID.

Resource ports SHALL use only these channel kinds: `FLUID`, `EU`, `HU`, `RU`,
and `ITEM`. A channel kind SHALL NOT encode a particular fluid or item. Ports
SHALL NOT have resource filters in this change: concrete transfer requests carry
`fluid_id` for FLUID or `item_id` for ITEM, and the network enforces the
applicable channel rules. An ITEM output port may therefore emit different
concrete `item_id` values over time; no wildcard, allow-list, or exact-item
selector is introduced.

#### Scenario: Pipe and cable references resolve to items
- **GIVEN** a row in `pipes.csv` or `cables.csv`
- **WHEN** the shared registry loads the row
- **THEN** its `item_id` resolves through `items.csv`
- **AND** the referenced item name matches the property row name
- **AND** the canonical packed ID is used by simulation, transport, and client code

#### Scenario: Drop references resolve to items
- **GIVEN** a row in `drops.csv`
- **WHEN** the shared registry validates the row
- **THEN** both the source block and result item resolve through `items.csv`
- **AND** an unknown or duplicate reference is a hard validation error

#### Scenario: Fluid has separate resource and item identities
- **GIVEN** the Steam row in `fluids.csv`
- **WHEN** a transport message is built
- **THEN** it uses the canonical `fluid_id` from `fluids.csv`
- **AND** any bucket/item representation uses the row's `item_id` from `items.csv`
- **AND** no packed item ID is used as a fluid ID by accident

#### Scenario: Registry validation fails before serving
- **GIVEN** duplicate canonical IDs, missing references, duplicate names, or
  mismatched names across registry files
- **WHEN** a service starts
- **THEN** registry validation reports a hard error
- **AND** the service does not accept world simulation traffic

### Requirement: Resource Data Ownership

Each mutable resource amount SHALL have exactly one authoritative owner. SimulationCore
owns machine buffers; PipeNetwork owns fluid physically buffered in pipe nodes;
no service SHALL mirror and independently debit the same amount.

#### Scenario: Machine source is drained once
- **GIVEN** a boiler owns a Steam buffer and PipeNetwork needs Steam
- **WHEN** PipeNetwork requests a drain
- **THEN** SimulationCore validates and debits its buffer once
- **AND** returns the accepted amount
- **AND** a repeated request with the same request ID returns the original result
- **AND** no telemetry event performs a second debit

### Requirement: Server-Authoritative Resource State

The server SHALL publish typed machine/port resource state through Gateway. The
client SHALL apply validated state on its state/render boundary and SHALL NOT
read raw PipeNetwork protocol messages in the render loop, invent coordinate-key
encodings, or hardcode resource labels.

#### Scenario: Client renders a resource buffer
- **GIVEN** a server `ResourceBufferState` for a machine port
- **WHEN** the client receives the update
- **THEN** the update is applied through the client state store
- **AND** the label and units are resolved from the registry
- **AND** the render loop displays the stored state without querying PipeNetwork

