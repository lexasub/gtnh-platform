## MODIFIED Requirements

### Requirement: Block Change Broadcast
The system SHALL broadcast every committed block change to all connected client sessions except the source session.

#### Scenario: Non-source clients receive the update
- **GIVEN** player 1 and player 2 have active sessions
- **WHEN** player 1 breaks or places a block and SimulationCore publishes a `BlockChangedEvent` on `world.blocks.changed`
- **THEN** the Gateway verifies the payload with `VerifyBuffer<BlockChangedEvent>`
- **AND** forwards the raw payload as `GatewayMsg::kBlockUpdate` on the bulk connection of every other session whose interest covers the chunk
- **AND** player 2 sees the block appear in their local world

#### Scenario: Source player is not re-sent the change
- **GIVEN** the event `source_player_id` equals the sender session player id
- **WHEN** the Gateway processes `world.blocks.changed`
- **THEN** it SHALL skip relaying the change to the source session only
- **AND** the source player relies on the optimistic `BlockAck` they already received

#### Scenario: Interest filtering per session
- **GIVEN** multiple sessions with distinct `PlayerInterest` centers
- **WHEN** a block change arrives
- **THEN** the Gateway filters the fanout per session interest instead of the previous nullptr single-interest broadcast

### Requirement: Player Disconnect Announcement
The system SHALL announce each player departure individually when that client session disconnects.

#### Scenario: Leave is announced after position flush
- **GIVEN** player 2 disconnects while player 1 stays connected
- **WHEN** the control connection of player 2 closes
- **THEN** the Gateway flushes only player 2 last position
- **AND** publishes `player.left` (with id and position) on MessageRouter
- **AND** the session of player 1 is unaffected

#### Scenario: Simulation continues without the player
- **GIVEN** any subset of players has disconnected
- **WHEN** SimulationCore keeps ticking at 20 Hz
- **THEN** world systems (drills, machines, pipes) SHALL continue operating
- **AND** no per-player resource cleanup is performed by SimulationCore today (no `player.left` subscriber)

### Requirement: Player Reconnect Restoration
The system SHALL restore authoritative player state when a client session connects or reconnects, for each player independently.

#### Scenario: Inventory restored on join
- **GIVEN** a client establishes a control connection
- **WHEN** the Gateway publishes `player.joined` for that player
- **THEN** MetaDB SHALL push that player authoritative state (inventory, quests) through `player.inventory.update` / quest topics
- **AND** the Gateway routes these updates to that session only

#### Scenario: World state flows via compressed chunks
- **GIVEN** a client is connected
- **WHEN** `world.chunk.loaded.compressed` arrives for any chunk within that session interest
- **THEN** the Gateway SHALL verify and forward the `CompressedChunkData` payload as `GatewayMsg::kCompressedChunkData` on that session bulk connection
- **AND** the client rebuilds the chunk from the compressed data
