# Change: Implement Heat Management

## Why
Heat management covers 6-neighbor heat propagation, overheat→explosion, boiler steam conversion, coolant-based cooling, pipe heat transport, and UI warnings. Core of `userflow 03`.

Beam: **already ~80% implemented**. This doc tracks what's built, what's pending, and remaining gaps for an agent.

## Implementation Status

| Component | Status | File(s) |
|-----------|--------|---------|
| HeatTransferSystem (6-neighbor propagation, overheat detection, env. cooling) | ✅ Done | `simulation_core/ECS/Systems/HeatTransferSystem.{h,cpp}` |
| ExplosionSystem (critical overheat → explosion after delay) | ✅ Done — behavior moved to `implement-explosion-mechanics` | `simulation_core/ECS/Systems/ExplosionSystem.{h,cpp}` |
| BoilerSystem (water+heat→steam, `steam_solid_boiler`) | ✅ Done | `simulation_core/ECS/Systems/BoilerSystem.{h,cpp}` |
| BoilerSystem (`steam_heat_boiler` STEAM→HEAT converter) | ✅ Done | `BoilerSystem.cpp:55-91` — consumes STEAM, produces HEAT into `HeatIntakeComponent`, publishes HEAT node update |
| CoolantSystem (coolant items reduce heat) | ✅ Done (header-only) | `simulation_core/ECS/Systems/CoolantSystem.h` |
| Pipe heat API (setNodeHeat, distributeHeat) | ✅ API done, **not wired** | `pipe_network/PipeNetwork.{h,cpp}:631,648` |
| Boiler→PipeNetwork publish | ✅ Done (STEAM via PipeEnergyClient) | `BoilerSystem.cpp:61-76` |
| MachineWindow overheat UI (yellow/red) | ✅ Done | `game_client/UI/Windows/block/MachineWindow.{h,cpp}:215-221` |
| BlockEntityUpdate heatRatio propagation | ✅ Done | `MachineWindow.cpp:543`, `BlockEntityUpdateData.heatRatio` |
| System registration in main.cpp | ✅ Done | `simulation_core/main.cpp:81-88,213-215` |
| **Pipe heat transport WIRING** (simcore publishes HEAT to pipe network) | ✅ Done | `PipeNetworkService::handleNodeUpdate` routes HEAT/STEAM to `setNodeHeat()` (was: only ELECTRICITY wired) |
| **Pipe heat tick** (pipe_network calls distributeHeat in loop) | ✅ Done | per-network `distributeHeat()` tick in pipe_network main loop |
| **HeatLoss module** (loss + per-node temperature) | ✅ Done | `HeatLoss.h`; `distributeHeat()` applies resistance × distance loss and tracks `PipeNode::temperature` |
| **Coolant item ID** — resolved to real registered item | ✅ Done | `HeatConstants.h:12`: `ItemId::pack("0:11111:4")` (coolant_bucket, `items.csv:56`) |
| **Boiler conversion rate** — extracted to constant | ✅ Done | `HeatConstants.h:13`: `CONVERSION_RATE = 1` |

## What Changes (Remaining Work)

### Remaining Work (Not Yet Implemented)
- ~~Wire pipe heat transport~~ — **DONE**: `PipeNetworkService::handleNodeUpdate` routes `EnergyType_HEAT`/`EnergyType_STEAM` to `setNodeHeat()`; pipe_network main loop has a per-network `distributeHeat()` tick
- ~~HeatLoss module~~ — **DONE**: `HeatLoss.h` (per-edge resistance × distance loss, per-node temperature)
- ~~Replace coolant item placeholder 0x99999~~ — **DONE**: `HeatConstants.h:12` → `ItemId::pack("0:11111:4")` (coolant_bucket, registered in `items.csv:56`)
- ~~Expose boiler conversion rate~~ — **DONE**: `HeatConstants.h:13` (`CONVERSION_RATE`)
- ~~`steam_heat_boiler` converter~~ — **DONE**: STEAM→HEAT conversion in `BoilerSystem.cpp:55-91`

### Already Implemented (Documentation Reference)
- HeatTransferSystem: 6-neighbor heat propagation with spatial index (`HeatTransferSystem.cpp:25-113`)
- Overheat detection: WARNING at 90%, CRITICAL at 100% — multiblock-only (`HeatTransferSystem.cpp:115-135`)
- Environment cooling: base rate 4.0, water adjacency 3x multiplier (`HeatTransferSystem.cpp:139-194`)
- Coolant cooling: 50 heat per coolant item (`CoolantSystem.h:52-53`)
- Explosion: 60 ticks at CRITICAL → block destroyed (`ExplosionSystem.cpp:23`) — behavior tracked in `implement-explosion-mechanics` (counter-reset bug)
- Boiler: `steam_solid_boiler` consumes heat + water bucket → produces 1 STEAM per tick (`BoilerSystem.cpp:38-54`)
- HEAT producer publish: `GeneratorSystem` publishes HEAT node updates for `heat_generator` (`GeneratorSystem.cpp:67-82`)
- UI: yellow bar at heatRatio ≥ 0.9, red at ≥ 1.0 (`MachineWindow.cpp:216-221`)
- All systems registered in main.cpp `spawnECSSystems()` at lines 81-88 and 213-215

## Impact
- Affected specs: heat-management (new — no existing spec); explosion-mechanics (new change `implement-explosion-mechanics`, explosion requirement moved there)
- Affected code:
  - `src/services/simulation_core/` — HeatTransferSystem, BoilerSystem, CoolantSystem, ExplosionSystem
  - `src/services/pipe_network/` — PipeNetwork heat API, tick loop wiring
  - `src/services/game_client/` — MachineWindow UI warnings
- Cross-cutting: changes touch 3 services (simcore + pipe_network + game_client)
