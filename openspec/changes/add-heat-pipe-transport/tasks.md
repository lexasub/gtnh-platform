# Change: Heat pipe transport + pipe renderer machine connections

## 1. Registry & item id
- [x] 1.1 Add `1111:10:4,heat_pipe,,0` to `data/registry/items.csv` (pipes section)
- [x] 1.2 Add `BLOCK_ID_HEAT_PIPE = ItemId::pack("1111:10:4")` to `src/services/pipe_network/PipeBlockIds.h`

## 2. PipeNetwork node model
- [x] 2.1 `PipeNetwork.cpp` `addNode()`: `case BLOCK_ID_HEAT_PIPE: node.heatCapacity = 1000;`
- [x] 2.2 `PipeNetwork.cpp` `addNodeWithId()`: same heat capacity case

## 3. PipeNetworkService connectivity
- [x] 3.1 `connectNodeNeighbors()` gains `bool isHeat` param; compatibility =
      `isItem ? itemCapacity>0 : (isHeat ? heatCapacity>0 : fluidCapacity>0)`;
      for machine endpoints in heat mode require `node_states_[nid].type == EnergyType_HEAT`
- [x] 3.2 `handleBlockChanged()`: pass heat mode for `BLOCK_ID_HEAT_PIPE` (isItem stays item-only)
- [x] 3.3 `handleNodeUpdate()`: after registering a HEAT node, call
      `connectNodeNeighbors(mgr_id, x, y, z, 0, false, true, false)` (machine placed after pipe)
- [x] 3.4 `isPipeBlock()` unchanged (range-based, heat pipe inside `1111:10` range — verified via ItemId::isPipe)

## 4. SimulationCore heat sink + pull
- [x] 4.1 `BoilerSystem::tick()`: publish HEAT energy node (`is_sink=true`, `energy.current/capacity`)
      every tick for `1110:01:1`
- [x] 4.2 `BoilerSystem::tick()`: when `heat_stored < HEAT_SINK_REPLENISH_TARGET` and steam room > 0,
      `pipeClient_->sendConsumeRequest(node_id, x, y, z, HEAT, needed)`
- [x] 4.3 `MachineSystem::onConsumeResponse()`: for HEAT machines sync
      `HeatIntakeComponent.heat_stored = energy.current`

## 5. GameClient renderer
- [x] 5.1 `PipeMeshBuilder.h`: add `HEAT_PIPE` to `PipeType` enum (index 4, before `CABLE_TIN`)
- [x] 5.2 `BlockRenderRegistry.h`: add `isMachineBlock(id)` = `CAT_MACHINES` range
- [x] 5.3 `PipeMeshBuilder::detectConnections()`: connect face when neighbour is machine block
- [x] 5.4 `CableMeshBuilder::detectConnections()`: same machine-connection rule

## 6. Tests
- [x] 6.1 `pipe_network_test.cpp`: heat pipe node has `heatCapacity == 1000`
- [x] 6.2 `pipe_network_test.cpp`: HEAT source machine + heat pipe + HEAT sink machine →
      `distributeHeat()` moves heat across the pipe
- [x] 6.3 `test_ecs_systems.cpp`: boiler publishes HEAT sink node state / requests heat when low
      (BoilerSystem_heat_pipe_replenish_request, offline router drops silently)

## 7. Verify & close
- [x] 7.1 `ninja -j5` + `ctest --output-on-failure -j$(nproc)` all green (16/16)
- [x] 7.2 `openspec validate add-heat-pipe-transport --strict`
- [x] 7.3 Committed (bef9141..ff966f6); heat pipe HEAT sink node published every tick — no
      remaining deferred work
