## ADDED Requirements
### Requirement: Per-Face Pipe/Cable Connection Masking
The system SHALL honor a per-face connection mask for cables, item pipes, and fluid pipes, so that a face disabled by the wrench (stored in the block meta byte, bits 0-5 = `{+X,-X,+Y,-Y,+Z,-Z}`, meta 0 meaning all six faces connected) does not form a transport edge. An edge between two adjacent nodes across face `f` SHALL be created only when the source node's mask has bit `f` set AND the neighbor node's mask has bit `f^1` set. Machine→pipe and machine→cable edges SHALL remain explicit and SHALL NOT be masked.

**References:**
- `src/services/pipe_network/CableGraph.cpp` — mask applied in `rebuildGraph`, `tick`, `findPath` (cables implemented)
- `src/services/pipe_network/PipeNetworkService.cpp` — `handleBlockChanged` (`pipe_meta_` cache), `handleItemNodeUpdate` / `handleFluidNodeUpdate` (item/fluid edge creation)
- `src/services/pipe_network/PipeNetwork.h` — `PipeNode.meta`, `CableNode.meta`
- `src/services/game_client/Render/PipeMeta.h` — `metaToFaceMask`, `detectConnections`

#### Scenario: Energy cable edge respects disabled face
- **GIVEN** two adjacent `cable_tin` nodes where the facing bit is cleared in one node's meta
- **WHEN** `CableGraph::rebuildGraph()` runs
- **THEN** no edge is created across that face
- **AND** energy does not flow between the two nodes through that face

#### Scenario: Item pipe machine→pipe edge respects disabled face
- **GIVEN** an item machine node adjacent to an item pipe whose connecting face is disabled in either block's meta
- **WHEN** `handleItemNodeUpdate` builds the machine→pipe edge
- **THEN** the edge is skipped
- **AND** items are not routed across the disabled face

#### Scenario: Item pipe↔pipe edge respects disabled face
- **GIVEN** two adjacent item pipe nodes with the shared face disabled in either node's meta
- **WHEN** the pipe↔pipe edge is evaluated during node registration or rebuild
- **THEN** no edge is created across that face

#### Scenario: Fluid pipe connectivity is built and respects the mask
- **GIVEN** a fluid machine adjacent to a fluid pipe
- **WHEN** `handleFluidNodeUpdate` builds connectivity
- **THEN** a fluid edge is created only if both nodes have the facing bits set
- **AND** a disabled face does not conduct fluid

#### Scenario: Meta 0 means all faces connected
- **GIVEN** an item/fluid pipe or cable with meta 0 (freshly placed)
- **WHEN** connectivity is evaluated
- **THEN** all six faces are treated as connected (backward compatible with pre-mask placement)
