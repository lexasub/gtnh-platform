## ADDED Requirements

### Requirement: Block Change Broadcast
The system SHALL broadcast all block changes to every connected client.

#### Scenario: Block change reaches all clients
- **GIVEN** player 1 places a block
- **WHEN** the block change is committed to ChunkStore
- **THEN** Gateway broadcasts the change to all connected clients
- **AND** player 2 sees the block appear in their world

### Requirement: Player Disconnect Cleanup
The system SHALL clean up player state on disconnect.

#### Scenario: Disconnect frees resources
- **GIVEN** a player disconnects from the server
- **WHEN** the disconnect is detected
- **THEN** player-bound ECS entities are freed
- **AND** player.disconnected is published on MessageRouter
- **AND** SimulationCore continues operating (drills, machines keep running)

### Requirement: Player Reconnect Restoration
The system SHALL restore player state on reconnect.

#### Scenario: Reconnect restores position and inventory
- **GIVEN** a previously connected player reconnects
- **WHEN** the TCP connection is established
- **THEN** the client receives current chunk data around login position
- **AND** inventory is loaded from MetaDB
- **AND** any autonomous progress (drills, machines) is visible

### Requirement: Service Communication Patterns
The system SHALL support pub/sub, RPC, and chained event patterns.

#### Scenario: Pub/sub fan-out
- **GIVEN** a service publishes to a topic
- **WHEN** multiple services are subscribed
- **THEN** all subscribers receive the message

#### Scenario: RPC request-response
- **GIVEN** a service sends an RPC request
- **WHEN** the target service processes it
- **THEN** the response is returned to the caller via MessageRouter
