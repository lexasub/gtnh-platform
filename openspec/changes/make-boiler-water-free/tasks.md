# Tasks: make-boiler-water-free

## 1. Solid boiler — coal → steam, no water
- [ ] 1.1 In `src/services/simulation_core/ECS/Systems/BoilerSystem.cpp`, delete the dead water-gated
      solid-boiler loop (`solidView`, lines 19–53). It gates on `container.slots[0]` water bucket
      `0:11111:0` (line 29) and swaps it to empty bucket `0:11111:3` (lines 33–35), but `heat_stored`
      is never populated for a `role: producer`, so the branch never fires. Coal→STEAM is owned by
      `GeneratorSystem`; remove this code to avoid contradictory/inert logic.
- [ ] 1.2 In `GeneratorSystem.cpp`, add a STEAM node-publishing branch mirroring the HEAT/ELECTRICITY
      branches (lines 70–103): when `energy.type == EnergyType::STEAM`, call
      `pipeClient_->publishNodeUpdate(...)` with `is_source=true`, `is_sink=false`, and
      `type = STEAM`. This makes the solid boiler's coal→STEAM visible to the PipeNetwork.
- [ ] 1.3 In `data/registry/machines.yaml` (lines 133–141), confirm `steam_solid_boiler` keeps
      `slots: { input: 2, output: 1 }`, `energy_out: STEAM`, `role: producer`, `max_output: 32`.
      No slot removal; coal stays in input slot 0.
- [ ] 1.4 Confirm `GeneratorSystem::isGenerator()` (line 12) still lists `1110:01:0` and coal fuel
      `0:11110:2` (line 18) is unchanged.

## 2. Heat boiler — neighbour heat → steam
- [ ] 2.1 In `data/registry/machines.yaml` (lines 143–153), change `steam_heat_boiler`: set
      `energy_in: HEAT`, `role: consumer`, and remove `energy_out: HEAT` (or set
      `energy_out: STEAM`). This makes `AdjacencyTransferSystem` deliver neighbour heat into its
      `EnergyStorage` (type HEAT) and sync `HeatIntakeComponent.heat_stored` — identical to
      `heat_furnace` (lines 37–46).
- [ ] 2.2 In `BoilerSystem.cpp`, rewrite the heat-boiler loop (lines 55–91). Replace the STEAM→HEAT
      conversion with HEAT→STEAM: include `HeatIntakeComponent` in the view; if `heat_stored > 0`,
      deduct `min(HeatConstants::CONVERSION_RATE, heat_stored)`; produce STEAM; publish a STEAM source
      node. Remove `get_or_emplace<HeatIntakeComponent>` (the engine now attaches it as a HEAT
      consumer) and the STEAM consumption / HEAT production logic.
- [ ] 2.3 Add `SteamOutputComponent { int32_t current; int32_t capacity; }` (mirror
      `HeatIntakeComponent.h`) under
      `src/services/simulation_core/ECS/components/`. Accumulate produced STEAM there and read from it
      for `produceEnergy`-style clamping and the STEAM node publish. (Fallback if a component is
      rejected: publish `produced_this_tick` transiently instead.)
- [ ] 2.4 Ensure the heat boiler's `EnergyStorage` stays type HEAT (so `AdjacencyTransferSystem`
      recognises it as a consumer); verify the engine attaches both `HeatIntakeComponent` and the new
      `SteamOutputComponent` to `1110:01:1` entities (same attachment path used for
      `HeatIntakeComponent`).

## 3. Steam output to PipeNetwork (both boilers)
- [ ] 3.1 Verify both boilers call `pipeClient_->publishNodeUpdate(...)` with `is_source=true` and
      `type=STEAM`: solid boiler via `GeneratorSystem` (task 1.2), heat boiler via `BoilerSystem`
      (task 2.2/2.3). Steam reaches the network (MVP transport); no back-face eject.
- [ ] 3.2 Confirm `PipeEnergyClient::publishNodeUpdate` signature matches the call sites
      `(node_id, x, y, z, current, capacity, maxInput, maxOutput, tier, type, is_source, is_sink)` —
      no signature change expected.

## 4. Client UI — dual heat + steam bars
- [ ] 4.1 Extend `BlockEntityUpdate` in the FlatBuffers schema under `src/protocol/` with
      `steam_current:uint` and `steam_capacity:uint`; regenerate the generated headers
      (`*_generated.h`). Keep defaults so older readers ignore them.
- [ ] 4.2 Server-side, populate `steam_current` / `steam_capacity` for both boilers in
      `publishBlockEntityUpdate` calls: solid boiler from its STEAM `EnergyStorage`
      (`BoilerSystem.cpp:50` / `GeneratorSystem.cpp:139`); heat boiler from `SteamOutputComponent`
      (task 2.3).
- [ ] 4.3 In `MachineWindow.cpp`: in `OnNetworkUpdate` (around line 490) read `steam_current` /
      `steam_capacity` into `pendingUpdate_`; in `Render` (line 347) draw a STEAM bar via
      `RenderEnergyBarImpl(EnergyType::STEAM, steam_current, steam_capacity, ...)`, and keep the HEAT
      bar driven by `temperature` (heat ratio). For the heat boiler `temperature` reflects
      `heat_stored`/`heat_capacity`; for the solid boiler it reads ~0 (coal model has no external heat
      buffer).
- [ ] 4.4 Verify both bars render for `1110:01:0` and `1110:01:1`. Adjust `ResolveProgressStyle`
      (line 160) if a distinct boiler layout is wanted (currently `boiler` → FLAME, line 171).

## 5. Tests & validation
- [ ] 5.1 In `src/services/simulation_core/ECS/test/test_ecs_systems.cpp`, add/adjust tests:
      (a) solid boiler with coal (no water) → STEAM produced + STEAM node published;
      (b) heat boiler adjacent to a `heat_generator` → heat delivered via `AdjacencyTransferSystem`
      → STEAM produced; (c) heat boiler with no neighbour heat → idle, no steam.
- [ ] 5.2 Add a UI/serialisation check that `BlockEntityUpdate` carries `steam_current` /
      `steam_capacity` and the client parses both bars for both boiler IDs.
- [ ] 5.3 Run `openspec validate make-boiler-water-free --strict` and fix issues.
- [ ] 5.4 Build + test: `cd cmake-build-debug && ninja -j5` then
      `ctest --output-on-failure -j$(nproc)`.
