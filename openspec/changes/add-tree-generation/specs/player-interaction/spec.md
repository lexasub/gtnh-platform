## MODIFIED Requirements

### Requirement: World Exploration
The system SHALL load chunks on demand from the client, generate them on miss, and cache them client-side.

#### Scenario: Client requests chunks on demand
- **GIVEN** the client camera moves into a new area
- **WHEN** `ChunkLoadManager::RunLoadPass` runs (rate-limited to 30 Hz)
- **THEN** it SHALL iterate the VIEW_RADIUS=8 × VERTICAL_RADIUS=3 cube around the camera chunk, frustum-cull candidates, and score them by distance/look-direction/velocity
- **AND** it SHALL send `PlayerAction{action=CHUNK_REQUEST, pos=Vec3i(cx,cy,cz)}` (kPlayerAction=1, ctrl port) for each chunk not already loaded or pending

#### Scenario: Gateway relays chunk requests
- **GIVEN** the gateway receives a `PlayerAction` from the client ctrl connection
- **WHEN** the action type is CHUNK_REQUEST
- **THEN** gateway SHALL publish it on topic `chunk.requests`
- **AND** SHALL drop MOVE/UNLOAD actions (client-driven loading; no server interest management)

#### Scenario: Chunk served from cache or LMDB
- **GIVEN** ChunkStore receives a chunk request on `chunk.requests`
- **WHEN** the chunk is in the lock-free CLOCK cache (1024 entries)
- **THEN** ChunkStore SHALL encode it to wire format and respond
- **AND** if not cached, SHALL read the chunk from LMDB (mmap, zero-copy)
- **AND** SHALL publish the encoded palette as `CompressedChunkData` (coord + palette_data) on topic `world.chunk.loaded.compressed`

#### Scenario: Chunk generated on cache miss
- **GIVEN** a requested chunk is neither in cache nor in LMDB
- **WHEN** `ChunkStore::AsyncGetChunk` calls `gen_queue_->requestChunk`
- **THEN** one of 8 generation worker threads SHALL run `WorldGenerator::GenerateTerrain` (2D Perlin terrain + 3D Simplex caves + ore veins + deterministic oak trees)
- **AND** tree generation SHALL be deterministic per the `tree-generation` capability (no inter-chunk state; consistent across horizontal and vertical chunk borders)
- **AND** the result SHALL be encoded, cached, and written to LMDB in batches before the pending request callback fires

#### Scenario: Client receives and meshes chunk
- **GIVEN** gateway receives `CompressedChunkData` on `world.chunk.loaded.compressed`
- **WHEN** it forwards to the client on the bulk connection as `kCompressedChunkData` (12)
- **THEN** the client SHALL build a `ChunkView` from the palette data, store it in `ChunkStorage` (via `World::OnChunkData`), and rebuild the mesh on a TBB worker
- **AND** any block updates received before the chunk arrived SHALL be replayed over the fresh snapshot
- **AND** the mesh SHALL be uploaded to bgfx GPU buffers via `ProcessPendingOps`

#### Scenario: Client-side chunk eviction with TTL
- **GIVEN** the loaded chunk count exceeds MAX_CHUNKS=1024
- **WHEN** `ChunkLoadManager` selects the farthest chunks for eviction
- **THEN** they SHALL enter an eviction queue with a 5-second TTL
- **AND** re-access within the TTL SHALL cancel eviction
- **AND** after TTL expiry the chunk SHALL be evicted and an UNLOAD `PlayerAction` SHALL be sent (dropped by gateway — server never evicts)
