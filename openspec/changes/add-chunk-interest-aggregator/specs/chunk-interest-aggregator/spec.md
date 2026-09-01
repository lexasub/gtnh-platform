## ADDED Requirements

### Requirement: Chunk Interest Aggregator
The platform SHALL provide a `chunk_interest` service that owns the chunk loaded/unloaded
status and fans out filtered, batched notifications about chunk contents to subscribers who
registered item-id ranges of interest.

#### Scenario: Client registers interest in pipes
- **WHEN** pipenetworkd registers an interest range for pipe item-ids (e.g. 33..60)
- **THEN** the aggregator records the range keyed by client and chunk scope

#### Scenario: Chunk loads containing a matching block
- **WHEN** a chunk is loaded and its decoded palette contains a block whose id falls in a
  subscriber's registered range
- **THEN** the aggregator schedules a batched `ChunkBatchNotify` for that subscriber (debounced,
  not one event per block)

#### Scenario: Batched delivery
- **WHEN** multiple matching blocks are found within the debounce window
- **THEN** the aggregator coalesces them into a single `ChunkBatchNotify` carrying all findings

#### Scenario: Explicit block change duplicating to aggregator
- **WHEN** simulation publishes a `world.blocks.changed` it also sends to chunkd
- **THEN** the aggregator receives the same change and notifies interested subscribers without a
  full chunk re-decode

### Requirement: Chunk Unload Notification
The aggregator SHALL notify subscribers when a chunk is unloaded, including the chunk bounds, so
they can evict local entities or edges.

#### Scenario: Pipe network backs on unload
- **WHEN** a chunk leaves the loaded set
- **THEN** pipenetworkd receives `ChunkUnloadNotify` with the chunk bounds and evicts tube nodes
  outside the surviving network

### Requirement: Chunk Ownership Transfer
The loaded/unloaded status of a chunk SHALL be owned by the `chunk_interest` service; simulation
SHALL defer to it rather than maintaining its own duplicate status.

#### Scenario: Single source of truth
- **WHEN** the aggregator decides a chunk is unloaded
- **THEN** simulation and pipe network learn of the transition via notification, avoiding
  divergent loaded-state
