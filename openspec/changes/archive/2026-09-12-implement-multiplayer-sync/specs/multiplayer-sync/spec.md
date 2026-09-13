## ADDED Requirements

### Requirement: Block Change Broadcast
The system SHALL broadcast every committed block change to all connected clients except the source player.

#### Scenario: Non-source clients receive the update
- **GIVEN** player 1 breaks or places a block
- **WHEN** SimulationCore publishes a `BlockChangedEvent` on `world.blocks.changed`
- **THEN** the Gateway verifies the payload with `VerifyBuffer<BlockChangedEvent>`
- **AND** forwards the raw payload to every connected client as `GatewayMsg::kBlockUpdate` on the bulk connection
- **AND** player 2 sees the block appear in their local world

#### Scenario: Source player is not re-sent the change
- **GIVEN** the event's `source_player_id` equals the gateway-side client player id
- **WHEN** the Gateway processes `world.blocks.changed`
- **THEN** it SHALL skip relaying the change back to the source player
- **AND** the source player relies on the optimistic `BlockAck` they already received

#### Scenario: Interest filtering is a full broadcast until implemented
- **GIVEN** `Gateway::client_interest()` still returns nullptr (TODO placeholder)
- **WHEN** a block change or chunk payload arrives
- **THEN** the payload SHALL be broadcast to all connected clients regardless of their position

### Requirement: Player Disconnect Announcement
The system SHALL announce player departure when a client disconnects.

#### Scenario: Leave is announced after position flush
- **GIVEN** a player disconnects from the server
- **WHEN** the control connection closes
- **THEN** the Gateway flushes the player's last position
- **AND** publishes `player.left` (with id and position) on MessageRouter

#### Scenario: Simulation continues without the player
- **GIVEN** a player has disconnected
- **WHEN** SimulationCore keeps ticking at 20 Hz
- **THEN** world systems (drills, machines, pipes) SHALL continue operating
- **AND** no per-player resource cleanup is performed by SimulationCore today (no `player.left` subscriber)

### Requirement: Player Reconnect Restoration
The system SHALL restore authoritative player state when a client connects or reconnects.

#### Scenario: Inventory restored on join
- **GIVEN** a client establishes a control connection
- **WHEN** the Gateway publishes `player.joined` eagerly
- **THEN** MetaDB SHALL push the player's authoritative state (inventory, quests) through `player.inventory.update` / quest topics

#### Scenario: World state flows via compressed chunks
- **GIVEN** a client is connected
- **WHEN** `world.chunk.loaded.compressed` arrives for any chunk
- **THEN** the Gateway SHALL verify and forward the `CompressedChunkData` payload as `GatewayMsg::kCompressedChunkData` on the bulk connection
- **AND** the client rebuilds the chunk from the compressed data

### Requirement: Service Communication Patterns
The system SHALL support pub/sub, RPC, and chained event patterns over MessageRouter.

#### Scenario: Pub/sub fan-out
- **GIVEN** a service publishes to a topic
- **WHEN** multiple services are subscribed to that topic
- **THEN** every subscriber receives the message (1-to-N fan-out)

#### Scenario: RPC request-response
- **GIVEN** a service sends an RPC request with a request id
- **WHEN** the target service processes it
- **THEN** the response SHALL be returned to the caller via MessageRouter, correlated by request id

#### Scenario: Chained pub/sub
- **GIVEN** a handler consumes an event (e.g. `BlockChanged`)
- **WHEN** the handler produces derived state (e.g. mesh invalidation, network re-solve)
- **THEN** the derived event is published for downstream subscribers (event → handler → new event)