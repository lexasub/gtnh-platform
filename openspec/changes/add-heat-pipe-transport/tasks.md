# Change: Heat pipe transport + pipe renderer machine connections

## 1. Registry & item id
- [ ] 1.1 Add `1111:10:4,heat_pipe,,0` to `data/registry/items.csv` (pipes section)
- [ ] 1.2 Add `BLOCK_ID_HEAT_PIPE = ItemId::pack("1111:10:4")` to `src/services/pipe_network/PipeBlockIds.h`

## 2. PipeNetwork node model
- [ ] 2.1 `PipeNetwork.cpp` `addNode()`: `case BLOCK_ID_HEAT_PIPE: node.heatCapacity = 1000;`
- [ ] 2.2 `PipeNetwork.cpp` `addNodeWithId()`: same heat capacity case

## 3. PipeNetworkService connectivity
- [ ] 3.1 `connectNodeNeighbors()` gains `bool isHeat` param; compatibility =
      `isItem ? itemCapacity>0 : (isHeat ? heatCapacity>0 : fluidCapacity>0)`;
      for machine endpoints in heat mode require `node_states_[nid].type == EnergyType_HEAT`
- [ ] 3.2 `handleBlockChanged()`: pass heat mode for `BLOCK_ID_HEAT_PIPE` (isItem stays item-only)
- [ ] 3.3 `handleNodeUpdate()`: after registering a HEAT node, call
      `connectNodeNeighbors(mgr_id, x, y, z, 0, false, true, false)` (machine placed after pipe)
- [ ] 3.4 `isPipeBlock()` unchanged (range-based, heat pipe already inside `1111:10` range — verify)

## 4. SimulationCore heat sink + pull
- [ ] 4.1 `BoilerSystem::tick()`: publish HEAT energy node (`is_sink=true`, `energy.current/capacity`)
      every tick for `1110:01:1`
- [ ] 4.2 `BoilerSystem::tick()`: when `heat_stored < HEAT_REPLENISH_THRESHOLD` and steam room > 0,
      `pipeClient_->sendConsumeRequest(node_id, x, y, z, HEAT, needed)`
- [ ] 4.3 `MachineSystem::onConsumeResponse()`: for HEAT machines sync
      `HeatIntakeComponent.heat_stored = energy.current`

## 5. GameClient renderer
- [ ] 5.1 `PipeMeshBuilder.h`: add `HEAT_PIPE` to `PipeType` enum (index 4, before `CABLE_TIN`)
- [ ] 5.2 `BlockRenderRegistry.h`: add `isMachineBlock(id)` = `CAT_MACHINES` range
- [ ] 5.3 `PipeMeshBuilder::detectConnections()`: connect face when neighbour is machine block
- [ ] 5.4 `CableMeshBuilder::detectConnections()`: same machine-connection rule

## 6. Tests
- [ ] 6.1 `pipe_network_test.cpp`: heat pipe node has `heatCapacity == 1000`
- [ ] 6.2 `pipe_network_test.cpp`: HEAT source machine + heat pipe + HEAT sink machine →
      `distributeHeat()` moves heat across the pipe
- [ ] 6.3 `test_ecs_systems.cpp`: boiler publishes HEAT sink node state / requests heat when low
      (extend BoilerSystem test with a capturing PipeEnergyClient if available, else manager-level)

## 7. Verify & close
- [ ] 7.1 `ninja -j5` + `ctest --output-on-failure -j$(nproc)` all green
- [ ] 7.2 `openspec validate add-heat-pipe-transport --strict`
- [ ] 7.3 `bd` issue for remaining work (if any); commit + push
