# Change: Implement Pipes & Cables Transport

## Why
Item pipes, fluid pipes, and energy cables are the core transport layer between machines. CableGraph + PipeNetworkManager are already implemented server-side (PipeNetworkService ~640 lines), PipeMeshBuilder + CableMeshBuilder on the client. What's missing: actual pipe block → machine inventory insertion, fluid→machine FLUID_IN/FLUID_OUT integration, transformers ↔ cable graph wiring, item buffer persistence via EntityStateStore, and dense pipe variant capacity distinction.

## What Changes

### Already Implemented ✅
- Pipe block IDs registered in `PipeBlockIds.h` (item=62, fluid=61, dense_item=64, dense_fluid=65)
- `isPipeBlock()` in `PipeNetworkService.cpp:276` — switch on BLOCK_ID_* constants
- `isCableBlock()` in `CableTypes.h:27` — checks CABLE_DEFS map
- Cable IDs in `data/registry/items.csv` (tin=66, copper=67, gold=68, alu=69, tungsten=70, platinum=71)
- Pipe IDs in `data/registry/items.csv` (fluid=61, item=62, dense_item=64, dense_fluid=65)
- `CableGraph` (`src/services/pipe_network/CableGraph.h/.cpp`) — packet-based energy BFS, add/remove nodes, `findPath()`, `injectPacket()`
- `CableLoss.h` — `effectiveVoltage()` loss calculation per block
- `CableGraph::processOverheat()` — overheat detection + `getExplodedNodes()`
- `PipeNetworkManager` (`src/services/pipe_network/PipeNetwork.cpp`) — `addNode()`, `removeNode()`, `rebuildItemNetworks()`, BFS traversal
- `PipeNetworkService` — `handleBlockChanged()` for auto-detection, `handleNodeUpdate()`, `handleFluidNodeUpdate()`, `handleItemNodeUpdate()`
- `TransformerSystem` (`src/services/simulation_core/ECS/Systems/TransformerSystem.cpp`) — step-up/down between tiers
- `PipeMeshBuilder` (`src/services/game_client/Render/PipeMeshBuilder.h`) — `PipeType` enum, `detectConnections()`, `buildPipeMesh()`
- `CableMeshBuilder` — cable-specific rendering with tier colors
- `BlockRenderRegistry.h` — `isPipeBlock()`, `isCableBlock()`, `blockIdToPipeType()`, `blockIdToCableTier()`
- Protocol: `src/protocol/pipe_network.fbs` — EnergyNodeUpdate, FluidNodeUpdate, ItemNodeUpdate, flow events, CableExplodedEvent

### Still Needs Implementation 🔴
- **Pipe → machine input slot insertion**: items travel through pipe network but machine `INPUT` slot integration not wired (`simulation_core` inventory system ↔ `pipe_network` item routing)
- **Fluid→machine FLUID_IN/FLUID_OUT**: PipeNetworkManager tracks `fluidBuffer`/`fluidCapacity`/`fluidId` per node, but machine fluid I/O roles not connected
- **Dense pipe capacity**: `dense_item_pipe` (block_id=64) and `dense_fluid_pipe` (block_id=65) registered but no capacity differentiation in BFS
- **Transformers ↔ CableGraph**: `TransformerSystem.cpp` exists, not wired to `CableGraph` for packet-based energy routing
- **Item buffer persistence**: no EntityStateStore persistence for item-in-transit
- **Cable explosion propagation**: `CableGraph::getExplodedNodes()` collected but no `SetBlock` back to air in ChunkStore
- **Side config → pipe routing**: WrenchHandler cycles `side_config` per face, but `PipeNetworkService::handleMachineConfigUpdated()` stub not routing by side_config
- **Tests**: `pipe_network_test.cpp` has CableGraph tests, no PipeNetworkManager tests, no integration tests

## Impact
- Affected specs: `pipes-cables-transport` (new capability)
- Affected code:
  - `src/services/pipe_network/PipeNetworkManager.cpp` — item/fluid BFS, hop routing
  - `src/services/pipe_network/CableGraph.cpp` — energy packet routing, overheat
  - `src/services/pipe_network/PipeNetworkService.h/.cpp` — block change handler, protocol handlers
  - `src/services/pipe_network/PipeBlockIds.h` — block ID constants
  - `src/services/simulation_core/ECS/Systems/TransformerSystem.cpp` — step-up/down
  - `src/services/simulation_core/` — machine I/O integration (inventory, fluid roles)
  - `src/services/entity_state_store/` — item buffer persistence
  - `src/services/game_client/Render/PipeMeshBuilder.h` — pipe/cable rendering
  - `src/services/game_client/Render/BlockRenderRegistry.h` — render block classification
  - `src/protocol/pipe_network.fbs` — FlatBuffers protocol messages
  - `data/registry/items.csv` — block ID registration
