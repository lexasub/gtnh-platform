## 0. Prerequisites & Context
- [x] 0.1 Pipe block IDs registered (`PipeBlockIds.h`: fluid=61, item=62, dense_item=64, dense_fluid=65)
- [x] 0.2 Cable block IDs registered (`CableTypes.h`/`items.csv`: tin=66..platinum=71)
- [x] 0.3 `isPipeBlock()` implemented in `PipeNetworkService.cpp:276`
- [x] 0.4 `isCableBlock()` implemented in `CableTypes.h:27`
- [x] 0.5 Block change handler subscribed to `world.blocks.changed` in `PipeNetworkService.cpp`
- [x] 0.6 Protocol messages defined in `src/protocol/pipe_network.fbs`

## 1. Item Pipes
- [x] 1.1 Block IDs registered (PipeBlockIds.h)
- [x] 1.2 isPipeBlock() implemented
- [x] 1.3 Item pipe BFS graph (`rebuildItemNetworks()` in PipeNetworkManager)
- [x] 1.4 PushItemToPipe: machine output → pipe (PipeNetwork `addNode()` with itemBuffer)
- [x] 1.5 Item movement: 1 block/tick along pipe path (`findNextItemHop()`)
- [x] 1.6 Insert into machine: pipe → machine input slot
  - **File**: `src/services/simulation_core/ECS/Reactors/ItemFlowHandler.cpp` — delivers item to first available machine input slot, sends `SetMachineSlotReq`, persists via EntityStateStore
  - **Detail**: PipeNetworkService publishes `item.flow` for consumed items → `ItemFlowHandler` receives → checks side_config INPUT/NONE face → delivers to `InventoryContainer` slot (empty slot or stack) → `SetMachineSlotReq` + EntityStateStore save
  - **Test**: `test_machine_to_pipe_to_machine()` verified in pipe_network_test

## 2. Fluid Pipes
- [x] 2.1 Fluid pipe BFS graph (PipeNetworkManager `addNode()` with fluid fields)
- [x] 2.2 Fluid movement along pipes (fluidBuffer/fluidCapacity tracking)
- [x] 2.3 Connect fluid I/O to machine FLUID_IN/FLUID_OUT roles
  - **File**: `src/services/pipe_network/PipeNetworkService.cpp` — `handleFluidNodeUpdate()`
  - **File**: `src/services/simulation_core/ECS/Reactors/FluidFlowHandler.cpp` — handles fluid delivery to `FluidStorage` component or STEAM `EnergyStorage`
  - **File**: `src/services/simulation_core/ECS/components/FluidStorage.h` — new FluidStorage component for machines with FLUID_IN roles
  - **Detail**: `FluidFlowHandler` delivers fluid to machine FluidStorage (FLUID_IN face checked) or STEAM energy storage. BoilerSystem produces STEAM energy → route via PipeEnergyClient → PipeNetworkService → FluidFlowHandler handles consumption
  - **Test**: `test_fluid_routing_capacity()`, `test_fluid_routing_type_mismatch()` verified in pipe_network_test

## 3. Energy Cables
- [x] 3.1 Cable block IDs + CABLE_DEFS (CableTypes.h)
- [x] 3.2 CableGraph add/remove/rebuild (CableGraph.h/.cpp)
- [x] 3.3 Energy packet injection + collection (CableGraph::injectPacket/collectPackets)
- [x] 3.4 Voltage tier checking (maxSeenVoltage > maxVoltage → overheat)
- [x] 3.5 Cable overheat detection + explosion (CableGraph::processOverheat)
- [x] 3.6 Cable loss per block (CableLoss.h: effectiveVoltage)
- [x] 3.7 Ampacity tracking (packetsThisTick > ampacity → overheat)
- [x] 3.8 Generator/machine registration (registerGenerator/registerMachine)
- [x] 3.9 Cable explosion → SetBlock(air) in ChunkStore
  - **File**: `src/services/pipe_network/PipeNetworkService.cpp` — collect `getExplodedNodes()` after each `cable_graph_.tick()`
  - **File**: `src/services/chunk_store/` — `SetBlockRPC` or publish `world.blocks.changed` with block_id=0
  - **Detail**: After `cable_graph_.tick()`, iterate `getExplodedNodes()`, for each send `SetBlock(pos, air)` to ChunkStore. Publish `CableExplodedEvent` for client effects
  - **Test**: Overvolt cable → cable block replaced with air

## 4. Transformers
- [x] 4.1 TransformerSystem exists (`src/services/simulation_core/ECS/Systems/TransformerSystem.cpp`)
- [x] 4.2 Step-up conversion logic (MV→HV, HV→EV)
- [x] 4.3 Step-down conversion logic
- [x] 4.4 Transformer block IDs registered (`1110:11:0`, `1110:11:1`)
- [x] 4.5 Transformers wired to CableGraph for packet routing
  - **File**: `src/services/simulation_core/ECS/Systems/TransformerSystem.cpp` — publishes sink node update (input tier) + source node update (output tier) via `PipeEnergyClient`
  - **File**: `src/services/pipe_network/CableGraph.cpp` — tier-aware `findAdjacentCable(.., tier)`, `registerGenerator/machine(.., tier)` to bind transformer sides to correct-tier cables
  - **File**: `src/services/pipe_network/PipeNetworkService.cpp` — passes `st.tier` to CableGraph `registerGenerator()/registerMachine()`
  - **Detail**: TransformerSystem publishes two `EnergyNodeUpdate` messages: sink node (entity id, inputTier) + source node (entity id | 0x100000000, outputTier). PipeNetworkService::handleNodeUpdate registers ELECTRICITY nodes in CableGraph with tier-aware binding. `findAdjacentCable(.., tier)` finds cables matching the specified tier, enabling proper input/output cable separation
  - **Test**: `test_cable_graph_transformer_integration()` in pipe_network_test

## 5. Persistence
- [x] 5.1 Item buffer persistence via EntityStateStore
  - **File**: `src/services/pipe_network/PipeNetworkManager.cpp` — `exportItemBuffers()` / `importItemBuffers()` for per-node `itemBuffer` serialization
  - **File**: `src/services/pipe_network/PipeNetworkService.cpp` — interval-based file save + `loadPersistentState()` on startup restores buffers from `{PERSIST_DIR}/item_buffers.txt`
  - **Detail**: `PipeNetworkManager::exportItemBuffers()` collects all non-empty item buffers keyed by node_id. Interval save in `tick()` writes to file every 50 ticks (5s at 100ms/tick). `loadPersistentState()` in `Start()` restores on startup and deletes the file. Format: `nodeId:itemId,count;itemId,count|nodeId:...`
  - **Test**: `test_export_import_item_buffers()`, `test_persistence_load_unload_cycle()` in pipe_network_test

## 6. Side Config Integration
- [x] 6.1 PipeNetworkService routes by side_config per face
  - **File**: `src/services/pipe_network/PipeNetworkService.cpp` — `handleMachineConfigUpdated()`
  - **File**: `src/services/simulation_core/ECS/Systems/WrenchHandler` — side_config rotation
  - **Detail**: When a machine face is wrenched to INPUT/OUTPUT/FLUID_IN/FLUID_OUT, PipeNetworkService must respect that config when routing items/fluids/energy. Currently side_config is in-memory only
  - **Test**: Wrench machine face to disable output → pipe should not insert into that face

## 7. Tests
- [x] 7.1 CableGraph add/remove node test (`test_cable_graph_add_remove`)
- [x] 7.2 CableGraph packet routing test (`test_cable_graph_packet_routing`)
- [x] 7.3 CableGraph voltage limit test (`test_cable_graph_voltage_limit`)
- [x] 7.4 CableGraph loss test (`test_cable_graph_loss`)
- [x] 7.5 CableGraph heavy loss test (`test_cable_graph_heavy_loss`)
- [x] 7.6 CableGraph overheat explosion test (`test_cable_graph_overheat_explosion`)
- [x] 7.7 CableGraph ampacity overheat test (`test_cable_graph_ampacity_overheat`)
- [x] 7.8 PipeNetworkManager item BFS test
- [x] 7.9 PipeNetworkManager item hop routing test
- [x] 7.10 PipeNetworkManager fluid routing test
- [x] 7.11 Integration: pipe block place → auto-detection → network rebuild
- [x] 7.12 Integration: machine output → pipe → machine input
- [x] 7.13 Integration: transformer step-up → cable → consumer (`test_cable_graph_transformer_integration`)
- [x] 7.14 Integration: cable explosion → SetBlock(air) event (`test_cable_explosion_event`)
- [x] 7.15 Integration: pipe/cable persistence load/unload cycle (`test_persistence_load_unload_cycle`)
