## Design Context

### System Architecture

Pipes and cables form a graph-based transport layer spanning 3 domains:
1. **Energy (CableGraph)** — packet-based electricity, voltage tiers, overheat/explosion, loss
2. **Fluid (PipeNetworkManager)** — fluid volume distribution, fluid type tracking, FLUID_IN/FLUID_OUT roles
3. **Items (PipeNetworkManager)** — item BFS routing, 1 block/tick movement, machine INPUT insertion

### Service Boundaries

```
SimulationCore (ECS)
  │
  ├─ EnergyNodeUpdate (topic: energy.node.update)
  ├─ FluidNodeUpdate  (topic: fluid.node.update)
  ├─ ItemNodeUpdate   (topic: item.node.update)
  │
  ▼
PipeNetworkService
  ├─ CableGraph (energy packets)
  ├─ PipeNetworkManager (fluid + item BFS)
  │
  ├─ energy.check.response / energy.consume.response
  ├─ fluid.check.response / fluid.consume.response
  ├─ item.transfer.response
  │
  ▼
SimulationCore (machines consume/produce)
```

### Key Design Decisions

**Decision 1: Separate energy packet graph from item/fluid BFS**
- CableGraph is packet-based (each tick, packets injected at generators, collected at consumers)
- PipeNetworkManager is BFS-based (connected component detection, item hop-by-hop routing)
- Rationale: Energy needs per-packet voltage/amp tracking for loss and overheat; items just need pathfinding

**Decision 2: PipeNetworkManager as unified BFS for both fluid and items**
- Single `addNode()` with `fluidBuffer`/`fluidCapacity`/`fluidId` + `itemBuffer`/`itemCapacity` fields
- `rebuildItemNetworks()` filters nodes with `itemCapacity > 0` into item sub-networks
- Same for fluid — avoids duplicating graph traversal code

**Decision 3: Block auto-detection via world.blocks.changed**
- `PipeNetworkService::handleBlockChanged()` subscribes to topic `world.blocks.changed`
- On pipe/cable place → `addNode()` + `rebuildNetworks()` / `addCableNode()`
- On pipe/cable break → `removeNode()` / `removeCableNode()`
- No explicit block registration step needed beyond items.csv entries

**Decision 4: Transformers as ECS entities with TransformerComponent**
- `TransformerSystem` ticks in `simulation_core` ECS (separate from PipeNetworkService)
- Step-up: accumulates low-voltage packets → converts ratio → emits high-voltage packets
- Step-down: receives high-voltage → distributes as low-voltage
- Needs `PipeEnergyClient` bridge to `CableGraph` for actual packet routing

### Protocol Topics
See `src/protocol/pipe_network.fbs` for all message definitions:
| Topic | Direction | Purpose |
|-------|-----------|---------|
| `energy.node.update` | SimCore → PipeNet | Register/update energy node state |
| `energy.check.request` | Machine → PipeNet | Query available energy |
| `energy.check.response` | PipeNet → Machine | Response with available/deficit |
| `energy.consume.request` | Machine → PipeNet | Request energy consumption |
| `energy.consume.response` | PipeNet → Machine | Consumption result |
| `energy.flow` | PipeNet → * | Energy movement events |
| `energy.cable.exploded` | PipeNet → * | Cable overheat event |
| `fluid.node.update` | SimCore → PipeNet | Register/update fluid node state |
| `fluid.check.request` | Machine → PipeNet | Query available fluid |
| `fluid.check.response` | PipeNet → Machine | Fluid availability response |
| `fluid.consume.request` | Machine → PipeNet | Request fluid consumption |
| `fluid.consume.response` | PipeNet → Machine | Fluid consumption result |
| `fluid.flow` | PipeNet → * | Fluid movement events |
| `item.node.update` | SimCore → PipeNet | Register/update item node state |
| `item.transfer.request` | Machine → PipeNet | Request item transfer |
| `item.transfer.response` | PipeNet → Machine | Transfer result |
| `item.flow` | PipeNet → * | Item movement events |
| `world.blocks.changed` | ChunkStore → * | Block place/break notification |

### Block ID Ranges
See `data/registry/items.csv` and `src/services/pipe_network/PipeBlockIds.h`:
| Range | Block IDs | Type |
|-------|-----------|------|
| `1111:10:0` | 61 | `fluid_pipe` |
| `1111:10:1` | 62 | `item_pipe` |
| `1111:10:2` | 64 | `dense_item_pipe` |
| `1111:10:3` | 65 | `dense_fluid_pipe` |
| `1111:01:0..5` | 66-71 | `cable_tin` through `cable_platinum` |

### Risks / Trade-offs
- **Pipe inventory integration is the largest gap**: item pipe BFS works (`rebuildItemNetworks`, `findNextItemHop`), but actual machine insertion requires SimulationCore inventory system to emit `item.transfer.request` → PipeNetworkService routes → pipe delivers → `SetMachineSlotReq`
- **Fluid machine I/O**: MachineRegistry has `energy_in`/`energy_out` fields but no `fluid_in`/`fluid_out`. Fluid role info must be added or inferred from machine tiers
- **Dense pipe distinction**: Currently no capacity differentiation between regular and dense pipes in `PipeNetworkManager::addNode()`. Need to set `itemCapacity`/`fluidCapacity` based on block_id

## Migration Plan
1. Complete machine inventory ↔ pipe insertion (highest priority — unblocks item transport gameplay)
2. Wire machine fluid roles → PipeNetworkManager fluid routing
3. Connect TransformerSystem to CableGraph
4. Add persistence for in-transit items
5. Wire CableExplodedEvent → SetBlock(air) in ChunkStore
6. Implement side_config → pipe routing
7. Write integration tests

## Open Questions
- Should dense pipes have 2x/4x capacity vs regular? What's the GTNH reference?
- Should transformers require cable connection on both tiers, or auto-detect tier from neighbors?
- Should in-transit item persistence be real-time (every tick) or interval-based?
