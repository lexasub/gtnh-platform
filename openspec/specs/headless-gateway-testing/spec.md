# headless-gateway-testing Specification

## Purpose
TBD - created by archiving change add-headless-multiblock-gateway-tests. Update Purpose after archive.
## Requirements
### Requirement: Headless Gateway multiblock integration coverage
The project SHALL provide a GUI-free integration path that builds a registered multiblock through the production Gateway TCP protocol and observes its lifecycle.

#### Scenario: EBF formation is observed through Gateway
- **GIVEN** Router, Gateway, ChunkStore, and SimulationCore are running
- **AND** the test client places the exact registered EBF pattern at an isolated coordinate
- **AND** the controller block is placed last
- **WHEN** the client receives `BlockAck` responses and the type-23 `MultiblockCreatedEvent`
- **THEN** every placement SHALL have an accepted acknowledgement
- **AND** the created event SHALL contain the expected anchor and pattern type
- **AND** the first milestone SHALL not require non-zero `mb_id` metadata for every structure block; that contract is deferred until production metadata writes are available.

#### Scenario: EBF teardown is observed through Gateway
- **GIVEN** an EBF was formed by the headless integration client
- **WHEN** the client breaks a structural block and then the anchor through `SetBlockAction`
- **THEN** the first milestone SHALL break the anchor and verify that it becomes air
- **AND** the type-23 destroyed event SHALL be observed after anchor removal
- **AND** non-anchor retention/metadata cleanup SHALL remain follow-up work until production supports that contract.

### Requirement: Headless client protocol helpers
The headless test client SHALL provide reliable length-prefixed reads, request-correlated block acknowledgements, and decoders for multiblock lifecycle events without changing the production wire IDs.

#### Scenario: Interleaved Gateway pushes do not mis-correlate ACKs
- **GIVEN** Gateway sends a block update or multiblock event before the requested acknowledgement
- **WHEN** the test waits for a particular `request_id`
- **THEN** the helper SHALL consume complete frames and continue waiting
- **AND** it SHALL return only the acknowledgement matching the requested ID and expected status

#### Scenario: CLI prints multiblock lifecycle events
- **GIVEN** `gateway_cli` is connected to Gateway
- **WHEN** a type-23 created or destroyed event arrives
- **THEN** the CLI SHALL decode and print its controller identity and lifecycle kind

### Requirement: Multiblock persistence integration harness
The integration harness SHALL be able to start EntityStateStore with isolated storage and inspect the production type-4 multiblock state save/load boundary.

#### Scenario: Multiblock state is inspected after the production save boundary
- **GIVEN** EntityStateStore is running with an isolated LMDB directory
- **AND** SimulationCore saves a multiblock state after its production destruction/unload boundary
- **WHEN** the test issues the EntityStateStore FlatBuffers query for entity type 4
- **THEN** the returned state SHALL decode as `MultiblockState`
- **AND** the test SHALL verify the controller identity, pattern, anchor, and persisted slots/state

