# Change: Heat pipe transport + pipe renderer machine connections

## Why
A `steam_heat_boiler` cannot be connected to pipes in practice:

1. **No heat pipe block exists.** `data/registry/items.csv` only registers
   `fluid_pipe` / `item_pipe` / `dense_item_pipe` / `dense_fluid_pipe`
   (`1111:10:0..3`). The server-side heat transport is fully implemented
   (`setNodeHeat`, `distributeHeat`, `HeatLoss` — issues GTNH-b95/90d/c76/hyf),
   but there is no block whose `PipeNode.heatCapacity > 0`, so HEAT never
   flows into a pipe graph. A heat generator sits next to a boiler and heat
   only moves via 6-neighbour adjacency (`AdjacencyTransferSystem`); anything
   a pipe apart is unreachable.
2. **Pipe/cable renderer never draws a connection to a machine.**
   `PipeMeshBuilder::detectConnections` and `CableMeshBuilder::detectConnections`
   only match a neighbour whose block id equals the same pipe/cable type
   (`getBlock(...) == target`). A pipe placed against a boiler/generator
   renders as a stub with no flange — visually "the pipe won't connect".

## What Changes
- **Add `heat_pipe` block** (`1111:10:4`) to `data/registry/items.csv`
  (falls into `ItemId::isPipe` range automatically).
- **PipeNetwork**: `BLOCK_ID_HEAT_PIPE` constant; `addNode`/`addNodeWithId`
  give it `heatCapacity = 1000`; `connectNodeNeighbors` gains a heat mode
  (`heatCapacity > 0` compatibility) and heat pipes attach to machine nodes
  whose `node_states_.type == HEAT`; `handleNodeUpdate` (machine register
  path) calls `connectNodeNeighbors` for HEAT nodes so a machine placed
  after its pipe still links.
- **SimulationCore**: `steam_heat_boiler` publishes a HEAT sink node
  (`is_sink=true`) every tick and issues `EnergyConsumeReq` (HEAT) when its
  heat buffer runs low, so pipes deliver HEAT into the boiler; the consume
  response path syncs `HeatIntakeComponent.heat_stored = energy.current`
  for HEAT machines.
- **GameClient renderer**: `detectConnections` (pipe + cable) connects a
  face when the neighbour is any machine block (`CAT_MACHINES` range),
  not only a same-type pipe/cable. `PipeType::HEAT_PIPE` added to the enum
  so `1111:10:4` maps through the existing `blockIdToPipeType` math.
- Machine energy nodes (`blockId=1` via `addNodeWithId`) already gain
  `heatCapacity` through `setNodeHeat` (issue GTNH-b95), so no storage-side
  change is needed for the nodes themselves.

## Impact
- Affected specs: `heat-management`
- Affected code:
  - `data/registry/items.csv` (+1 row)
  - `src/services/pipe_network/PipeBlockIds.h`, `PipeNetwork.cpp`,
    `PipeNetworkService.cpp`
  - `src/services/simulation_core/ECS/Systems/BoilerSystem.cpp`,
    `src/services/simulation_core/ECS/Systems/MachineSystem.cpp`
  - `src/services/game_client/Render/PipeMeshBuilder.{h,cpp}`,
    `CableMeshBuilder.cpp`, `BlockRenderRegistry.h`
- Non-breaking: new block id, new render behaviour, no protocol change.
