## Context

Heat management touches three services:
1. **SimulationCore** — ECS systems (HeatTransfer, Boiler, Coolant, Explosion)
2. **PipeNetwork** — heat distribution through pipe graphs
3. **GameClient** — heat warnings in MachineWindow UI

The ECS systems and UI are already implemented. The pipe heat transport API exists but is not wired into either service's tick loop.

## Goals / Non-Goals

**Goals:**
- Document existing heat management architecture for agents
- Identify remaining wiring gaps precisely with file:line refs

**Non-Goals:**
- Redesign heat model — current values (KEV = 1, cooling = 4.0/tick) are playable
- Add new heat sources/sinks beyond PRODUCER/CONSUMER roles
- Scriptable heat mechanics — deferred

## Architecture

### Data Flow

```
                  HeatTransferSystem::tick()
                  ┌─────────────────────────────┐
                  │ Pass 1: Adjacent propagation │
                  │ Pass 2: Overheat detection    │
                  │ Pass 3: Environment cooling   │
                  └──────┬──────────────────────┘
                         │ HeatIntakeComponent.heat_stored
                         ▼
             ┌───────────────────────┐
             │    BoilerSystem       │ ◄── heat_stored
             │  (water+heat→STEAM)   │ ──► PipeEnergyClient → PipeNetwork
             └───────────────────────┘
                         │ OverheatComponent
                         ▼
             ┌───────────────────────┐
             │   CoolantSystem       │ ◄── coolant items → reduce heat
             └───────────────────────┘
                         │ OverheatComponent.state == CRITICAL
                         ▼
             ┌───────────────────────┐
             │   ExplosionSystem     │ → publishBlockChangedEvent (air)
             └───────────────────────┘
```

> `ExplosionSystem` behavior (60-tick countdown, block destruction) is specified in
> `implement-explosion-mechanics` — moved because `HeatTransferSystem` Pass 2 resets
> `ticks_at_critical` every tick, making the countdown unreachable in the current code.

### Pipe Heat Transport (the gap)

`PipeNetworkManager` has:
- `PipeNode.heatStored` / `heatCapacity` (`PipeNetwork.h:46-47`)
- `setNodeHeat()` (`PipeNetwork.cpp:631`) — sets node heat properties
- `distributeHeat()` (`PipeNetwork.cpp:648`) — moves excess heat >90% to sinks

**What's missing:**
1. **SimCore → PipeNetwork**: done. `GeneratorSystem.cpp:67-82` publishes `EnergyType::HEAT` node updates for `heat_generator`; `BoilerSystem.cpp:61-76` publishes STEAM.
2. **PipeNetwork ingestion**: `PipeNetworkService::handleNodeUpdate()` (`PipeNetworkService.cpp:298-348`) only wires `ELECTRICITY` nodes into CableGraph; HEAT/STEAM nodes are registered but `setNodeHeat()` is never called — `PipeNode.heatStored` stays 0.
3. **PipeNetwork tick**: no per-network distribution tick exists — `distributeHeat()` (and `distributeEnergy()`) are uncalled in the `pipe_network` main loop (`main.cpp:57-65`).

**Wiring approach** (decided):
- Reuse `PipeEnergyClient::publishNodeUpdate(energy_type = HEAT)` from heat producers — same pattern as `BoilerSystem.cpp:61-76`
- In `PipeNetworkService::handleNodeUpdate`, route `EnergyType_HEAT` to `setNodeHeat()`
- Add a per-network `distributeHeat()` call in the pipe_network main loop
- Loss/temperature computed by a dedicated `pipe_network/HeatLoss` module (mirrors `CableLoss.h`/`CableOverheat.h`)

### Tick Order

Systems in `main.cpp` registration order (`spawnECSSystems` + inline):
1. CoolantSystem
2. ExplosionSystem
3. GeneratorSystem
4. CreativeGeneratorSystem
5. BoilerSystem
6. TransformerSystem
7. DrillSystem
8. RotareGeneratorSystem
9. HeatTransferSystem
10. MachineSystem
11. BatteryBufferSystem

Coolant runs before HeatTransfer — means coolant can pre-cool before overheat detection re-evaluates. This is intentional.

## Key File References

| Component | File | Lines |
|-----------|------|-------|
| 6-neighbor propagation | `HeatTransferSystem.cpp` | 25-113 |
| Overheat detection | `HeatTransferSystem.cpp` | 115-135 |
| Environment cooling | `HeatTransferSystem.cpp` | 139-194 |
| Explosion tick | `ExplosionSystem.cpp` | 12-39 |
| Boiler water→steam | `BoilerSystem.cpp` | 25-91 |
| Coolant consumption | `CoolantSystem.h` | 20-64 |
| Constants | `HeatConstants.h` | 1-13 |
| PipeNode heat fields | `PipeNetwork.h` | 46-47 |
| setNodeHeat | `PipeNetwork.cpp` | 631-639 |
| distributeHeat | `PipeNetwork.cpp` | 648-754 |
| UI energy bar | `MachineWindow.cpp` | 195-233 |
| UI heatRatio parsing | `MachineWindow.cpp` | 543 |
| BlockEntityUpdateData | `MachineWindow.h` | 26-35 |
| System registration | `main.cpp` | 81-89, 213-229 |

## Risks / Trade-offs

- **Coolant item ID placeholder**: `0x99999` — must resolve to real registry ID before item can be obtained in-game
- **Hardcoded kConversionRate = 1**: fine for tuning pass; extract to YAML/config when multiple boiler types exist
- **No pipe heat wiring yet**: heat only flows through adjacent propagation, not through pipes. This is the main feature gap.
- **CoolantSystem is header-only**: fine for ~65 LOC; extract .cpp when complexity grows

## Decisions

- **Heat transport loss**: a dedicated `pipe_network/HeatLoss` module (mirroring `CableLoss.h`/`CableOverheat.h`) computes per-edge resistance × distance loss and per-node temperature. `distributeHeat()` keeps network-wide pooling (cap `MAX_HEAT_PER_TICK = 1000`) and consults `HeatLoss` for the effective transfer.
- **Client reuse**: extend `PipeEnergyClient::publishNodeUpdate(energy_type)` rather than creating a `PipeHeatClient` — the message already carries `EnergyType`.
- **Distribution tick**: add a per-network `distributeHeat()` call to the pipe_network main loop (none exists today); co-locate with the energy/fluid distribution once those are wired.
- **Explosion moved out**: the `Explosion on Critical Overheat` requirement moved to `implement-explosion-mechanics` because the current code resets `ticks_at_critical` to 0 every tick (`HeatTransferSystem.cpp:125`), making 60 ticks unreachable.
- **Boilers split**: `steam_solid_boiler` = water+heat→STEAM; `steam_heat_boiler` = STEAM→HEAT converter (per `data/registry/machines.yaml`). The converter logic is new work.
- **Coolant**: multiblock-only (`CoolantSystem` view requires `MultiblockController`); `COOLANT_ITEM_ID` must resolve to a real registered item — `0x99999` cannot fit in `uint16_t`.
