## Purpose

Define the communication protocol for GTNH Platform — wire format, message types, topic conventions, and delivery guarantees for all service-to-service and client-to-gateway communication.

## Requirements

### Requirement: Wire Format
All service-to-service and client-to-gateway communication SHALL use a length-prefixed binary frame format.

#### Scenario: Frame structure
- **GIVEN** a message is sent over TCP
- **THEN** the wire format SHALL be: `[4 bytes: big-endian payload size][payload]`
- **AND** the payload SHALL be a FlatBuffer

#### Scenario: Client-gateway framing
- **GIVEN** a GameClient communicates with Gateway
- **THEN** the wire format SHALL be: `[4 bytes size BE][1 byte msg_type][FlatBuffer]`
- **AND** `msg_type` SHALL dispatch message parsing without FlatBuffers union

### Requirement: Message Types (Client↔Gateway)
The client-gateway protocol SHALL define the following message types:

| Type ID | Name | Direction | Payload |
|---------|------|-----------|---------|
| 0 | PlayerAction | Client→Gateway | Player position, block break/place |
| 1 | ChunkData | Gateway→Client | Full chunk snapshot (192 KB) |
| 2 | EntitySnapshot | Gateway→Client | Entity state update |
| 3 | BlockAck | Gateway→Client | Block operation confirmation/rejection |
| 4 | BlockUpdate | Gateway→Client | Incremental block change |
| 5 | InventoryUpdate | Gateway→Client | Inventory snapshot/update |
| 6 | InventoryAction | Client→Gateway | Inventory operation (move, split, etc.) |
| 7 | CraftRequest | Client→Gateway | Craft from workbench |
| 8 | CraftResponse | Gateway→Client | Craft result |
| 9 | GridUpdate | Gateway→Client | Workbench grid state |
| 10 | BlockEntityUpdate | Gateway→Client | Machine/multiblock state |

#### Scenario: Player action dispatched to SimulationCore
- **GIVEN** Gateway receives a `PlayerAction` (type_id=0) from client
- **WHEN** the action contains a block placement
- **THEN** Gateway publishes it to `player.actions` topic on MessageRouter
- **AND** SimulationCore processes it

#### Scenario: BlockAck sent for each mutation
- **GIVEN** SimulationCore confirms a block change via ChunkStore
- **WHEN** the change is committed
- **THEN** Gateway sends a `BlockAck` (type_id=3) to the client
- **AND** the client updates its local state only on receipt

### Requirement: FlatBuffers Schema Organization
Protocol schemas SHALL be organized in `src/protocol/` as FlatBuffers `.fbs` files.

#### Scenario: Schema files and their purpose
- **GIVEN** the protocol is versioned
- **THEN** `core.fbs` SHALL define core types (Vec3i, ItemStack, Block, Chunk, PlayerAction)
- **AND** `gateway.fbs` SHALL define GatewayPayload union and GatewayMessage
- **AND** `chunkstore.fbs` SHALL define GetBlock/SetBlock/GetChunk RPC
- **AND** `simcore.fbs` SHALL define BlockChangedReq, MatchPatternReq, TickReq
- **AND** `recipe.fbs` SHALL define RecipeManager types (MachineType enum, CraftReq, etc.)
- **AND** `entity_state_store.fbs` SHALL define entity state persistence RPC
- **AND** `meta_db.fbs` SHALL define player inventory/position RPC
- **AND** `machine_state.fbs` SHALL define machine state RPC

### Requirement: MessageRouter Topic Conventions
MessageRouter SHALL use MQTT-style topic patterns for pub/sub.

#### Scenario: Topic naming convention
- **GIVEN** a message is published via MessageRouter
- **THEN** topics SHALL use dot-separated segments: `<domain>.<entity>.<action>`
- **AND** `+` SHALL match one segment (single-level wildcard)
- **AND** `#` SHALL match trailing segments (multi-level wildcard)

#### Scenario: Standard topics exist
- **GIVEN** the platform is running
- **THEN** the following topics SHALL be available:
  - `player.actions` — player actions from Gateway
  - `player.joined` / `player.left` — player connect/disconnect
  - `player.inventory.update` — inventory changes from MetaDB
  - `world.blocks.changed` — block mutations from ChunkStore
  - `world.chunk.loaded` — chunk load events
  - `entity.state.get` / `entity.state.set` — entity state RPC
  - `energy.node.update` — energy network changes
  - `energy.consume.request` / `energy.consume.response` — energy consumption
  - `energy.flow` — PipeNetwork flow results

### Requirement: MessageRouter Delivery Guarantees
MessageRouter SHALL provide different delivery guarantees for different message classes.

#### Scenario: At-least-once for state changes
- **GIVEN** a state-changing message is published (e.g., `world.blocks.changed`)
- **THEN** the receiver SHALL send an Ack
- **AND** MessageRouter SHALL retry delivery if no Ack is received

#### Scenario: At-most-once for streaming data
- **GIVEN** a streaming message is published (e.g., `EntitySnapshot`, `ChunkSnapshot`)
- **THEN** MessageRouter SHALL deliver at most once
- **AND** SHALL NOT retry on failure

#### Scenario: No self-delivery
- **GIVEN** a service publishes to a topic it is subscribed to
- **THEN** MessageRouter SHALL NOT deliver the message to the publisher
- **AND** this SHALL prevent infinite loops (e.g., SimulationCore publishing `world.blocks.changed`)

### Requirement: Service Discovery
MessageRouter SHALL maintain a registry of connected services and their capabilities.

#### Scenario: Service registration on connect
- **GIVEN** a service connects to MessageRouter
- **THEN** it SHALL send a registration message with its service name and supported message types
- **AND** MessageRouter SHALL route messages only to services that handle the corresponding type
