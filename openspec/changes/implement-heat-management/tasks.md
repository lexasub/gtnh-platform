## 0. Already Implemented (Verification Only)
- [x] 0.1 HeatTransferSystem: 6-neighbor heat propagation, overheat detection, env cooling — **DONE** (`HeatTransferSystem.cpp`)
- [x] 0.2 BoilerSystem: water+heat→steam conversion (`steam_solid_boiler`) — **DONE** (`BoilerSystem.cpp`)
- [x] 0.3 CoolantSystem: coolant items reduce heat — **DONE** (`CoolantSystem.h`)
- [x] 0.4 MachineWindow UI: yellow at 90%, red at 100% heat — **DONE** (`MachineWindow.cpp`)
- [x] 0.5 Pipe heat API: setNodeHeat(), distributeHeat() — **DONE** (but not wired)
- [x] 0.6 All heat systems registered in simcore main.cpp — **DONE**

> Note: `ExplosionSystem` is implemented and registered, but its behavior requirement now lives in
> `implement-explosion-mechanics` (the 60-tick counter is reset every tick in the current code).

## 1. Pipe Heat Transport Wiring (Remaining)
- [x] 1.1 Publish HEAT node updates from simcore heat producers via `PipeEnergyClient::publishNodeUpdate(energy_type = HEAT)` — **DONE** (`GeneratorSystem.cpp:67-82` for `heat_generator`; `BoilerSystem.cpp:61-76` publishes STEAM)
- [ ] 1.2 In `PipeNetworkService::handleNodeUpdate`, call `setNodeHeat()` for `EnergyType_HEAT` nodes (currently only `ELECTRICITY` is wired to CableGraph; `setNodeHeat` has no callers)
- [ ] 1.3 Add a per-network distribution tick to the pipe_network main loop calling `distributeHeat()` (the loop currently has no distribution tick at all)
- [ ] 1.4 Implement `pipe_network/HeatLoss` (mirroring `CableLoss.h`/`CableOverheat.h`) for per-edge resistance × distance loss and per-node temperature
- [ ] 1.5 Test: heat flows through pipes from producer to distant consumer, reduced by `HeatLoss`, capped at 1000/tick

## 2. Cleanup / Hardcoded Values
- [ ] 2.1 Resolve `HeatConstants::COOLANT_ITEM_ID` (currently `0x99999`, unrepresentable in `uint16_t`) to a real registered coolant item; add the item to the registry if missing
- [ ] 2.2 Extract `BoilerSystem::kConversionRate = 1` to config/constant
- [ ] 2.3 Implement `steam_heat_boiler` as a STEAM→HEAT converter per `data/registry/machines.yaml` (currently mis-tagged as water+heat→STEAM)

## 3. Edge Cases
- [ ] 3.1 Multiple boilers feeding one steam network — aggregate capacity
- [ ] 3.2 Coolant on non-overheated machine — no-op (already implemented: CoolantSystem checks OverheatComponent)
- [ ] 3.3 Coolant depletion: coolant stack → 0 → slot cleared (already implemented)
- [ ] 3.4 Coolant only on multiblocks — verify scenario wording matches `CoolantSystem` view (requires `MultiblockController`)
- [ ] 3.5 steam_heat_boiler converter: verify `HeatIntakeComponent` is attached when producing HEAT (SimulationEngine attaches it only for `EnergyType::HEAT` machines)

## 4. Verification
- [ ] 4.1 Build: `cd cmake-build-debug && ninja -j5` — no new compilation errors
- [ ] 4.2 Tests: `ctest --output-on-failure -j$(nproc)` — all pass
- [ ] 4.3 LSP diagnostics clean on changed files
