## ADDED Requirements

### Requirement: Multi-Client Sessions
The Gateway SHALL support 2 to 8 concurrent client sessions, each with independent ctrl and bulk connections and independent player identity.

#### Scenario: Two clients connect simultaneously
- **WHEN** a second client establishes ctrl+bulk connections while the first remains connected
- **THEN** the Gateway SHALL keep both sessions alive
- **AND** neither session loses its connection state or player identity

#### Scenario: Ctrl and bulk pairing
- **WHEN** a client presents the same bulk_token on ctrl and bulk handshakes
- **THEN** the Gateway SHALL join both connections into one session
- **AND** a bulk connection without a matching ctrl session SHALL be rejected after a timeout

### Requirement: Session-Scoped Player Identity
The Gateway SHALL assign and validate each session player id, and clients SHALL use the server-confirmed id for all actions.

#### Scenario: Server issues canonical id
- **WHEN** a client sends ClientHello with desired id and nickname
- **THEN** the Gateway confirms a canonical player id (resolving collisions)
- **AND** publishes player.joined with the id and nickname

#### Scenario: Spoofed player id rejected
- **GIVEN** session S is bound to player id 2
- **WHEN** a message from S carries player_id != 2
- **THEN** the Gateway SHALL drop the action

### Requirement: Addressed Replies
The Gateway SHALL route every router-to-client message to the session(s) of the intended recipient(s), using payload player_id or request-id correlation scoped per session.

#### Scenario: Identical request ids do not cross
- **GIVEN** two sessions issued requests with the same request_id
- **WHEN** responses arrive on the corresponding topics
- **THEN** each response is delivered only to the requesting session

#### Scenario: Private data stays private
- **WHEN** an inventory or quest update for player 2 is published
- **THEN** only the session of player 2 receives it

### Requirement: Per-Session Chunk Interest
Each session SHALL maintain its own chunk interest and the Gateway SHALL attribute chunk requests to the requesting player.

#### Scenario: Independent interests
- **GIVEN** players at distant positions
- **WHEN** a chunk loads near player 2
- **THEN** the chunk is sent to sessions whose interest covers it and not to distant sessions

#### Scenario: Chunk requests carry player id
- **WHEN** the Gateway re-requests chunks (e.g. after player.position.load)
- **THEN** the published chunk request carries the session player_id (never 0)

### Requirement: Remote Player Visibility
The system SHALL relay other players actions and positions so that every client observes a consistent world.

#### Scenario: Peer sees foreign block change
- **GIVEN** player 1 and player 2 are connected
- **WHEN** player 1 places a block
- **THEN** player 2 receives the BlockUpdate
- **AND** player 1 relies on the optimistic BlockAck (no echo)

#### Scenario: Peer sees foreign movement
- **WHEN** player 2 moves within the interest of player 1
- **THEN** player 1 receives position snapshots of player 2

### Requirement: Per-Session Lifecycle
The Gateway SHALL handle join, disconnect, and position restore per session without affecting other sessions.

#### Scenario: One player leaves, others keep playing
- **GIVEN** two active sessions
- **WHEN** player 1 disconnects
- **THEN** the Gateway publishes player.left for player 1 only
- **AND** the session of player 2 continues uninterrupted

#### Scenario: Position restore routed to owner
- **WHEN** player.position.load arrives for player 2
- **THEN** only the session of player 2 receives the chunk re-request consequence

### Requirement: Multi-Client Test Harness and Throughput Baseline
The project SHALL provide headless multi-client integration tests and record a throughput baseline for 2 to 8 clients.

#### Scenario: Harness isolates failures
- **WHEN** one client misbehaves or disconnects mid-test
- **THEN** other clients continue passing their assertions

#### Scenario: Baseline recorded
- **WHEN** the multi-client throughput scenario runs
- **THEN** msg/s per N clients (N=2..8) is recorded as a baseline artifact for the networking v2 stage
