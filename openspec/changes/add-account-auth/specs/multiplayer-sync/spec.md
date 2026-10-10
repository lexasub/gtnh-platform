## MODIFIED Requirements

### Requirement: Player Reconnect Restoration
The system SHALL restore authoritative player state when an authenticated client connects or reconnects.

#### Scenario: Inventory restored on authenticated join
- **GIVEN** a client has successfully authenticated to a stable account mapped to `player_id`
- **WHEN** the Gateway publishes `player.joined` for that player
- **THEN** MetaDB SHALL push that player's authoritative inventory and quest state using the mapped numeric `player_id`
- **AND** unauthenticated connections SHALL NOT trigger player state loading

#### Scenario: World state flows via compressed chunks
- **GIVEN** an authenticated client is connected
- **WHEN** `world.chunk.loaded.compressed` arrives for a chunk requested by that session
- **THEN** the Gateway SHALL verify and forward the `CompressedChunkData` payload as `GatewayMsg::kCompressedChunkData` on the paired bulk connection
- **AND** the client rebuilds the chunk from the compressed data
