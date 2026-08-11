# Design: make-boiler-water-free

## Context
Boilers are ECS machines in family `1110`:
- `1110:01:0` `steam_solid_boiler` — `energy_out: STEAM`, `role: producer`, `slots: {input:2, output:1}`
  (`data/registry/machines.yaml:133-141`). `GeneratorSystem::isGenerator()` includes it
  (`GeneratorSystem.cpp:12`), so it burns coal (`FuelValues` coal `0:11110:2` = 8000 at
  `GeneratorSystem.cpp:18`) and produces energy of type STEAM into its `EnergyStorage`
  (`GeneratorSystem.cpp:67`). The separate `BoilerSystem` solid branch (`BoilerSystem.cpp:19-53`)
  tries to consume a water bucket + `HeatIntakeComponent.heat_stored` → STEAM, but `heat_stored` is
  **never populated for a `producer`** (see below), so that branch is dead. Net current behaviour:
  the solid boiler makes STEAM from coal via `GeneratorSystem` only; the water gate is inert.
- `1110:01:1` `steam_heat_boiler` — currently `energy_in: STEAM`, `energy_out: HEAT`, `role: producer`
  (`machines.yaml:143-153`). `BoilerSystem.cpp:55-91` consumes STEAM from its `EnergyStorage` and
  writes HEAT into `HeatIntakeComponent`, publishing a HEAT node. This is inverted relative to
  intent and SHALL become HEAT→STEAM.

Heat delivery to a neighbour machine is handled by `AdjacencyTransferSystem::tick` (Pass 1,
`AdjacencyTransferSystem.cpp:25-118`), NOT the PipeNetwork. It scans `EnergyType::HEAT` machines:
`MachineRole::PRODUCER` (or matching `energy_out`) are sources; `MachineRole::CONSUMER` machines
pull heat from any of the 6 adjacent positions, and the system syncs
`HeatIntakeComponent.heat_stored = energy.current` (`AdjacencyTransferSystem.cpp:106-108`). This is
exactly how `heat_generator` (`1110:00:2`) already feeds `heat_furnace` (`1110:00:0`,
`machines.yaml:37-46`, `role: consumer`, `energy_in: HEAT`). There is no file literally named
`HeatTransferSystem` (the README's loose name); the real code is `AdjacencyTransferSystem`. A machine
can carry only ONE `EnergyStorage` component, so a boiler cannot simultaneously use its `EnergyStorage`
as a HEAT consumer input and as a STEAM output buffer.

## Goals / Non-Goals
- Goals: solid boiler makes STEAM from coal (no water); heat boiler makes STEAM from neighbour heat;
  boiler UI shows heat + steam; steam reaches the pipe network.
- Non-Goals: a `heat_generator` machine (already exists `1110:00:2`); back-face directional steam
  eject; finite water.

## Decisions
- Decision: `steam_solid_boiler` keeps its existing coal→STEAM path in `GeneratorSystem`; remove the
  dead water-gated `BoilerSystem` branch (`BoilerSystem.cpp:19-53`) entirely. No heat input — it burns
  coal. Slots unchanged (`machines.yaml:138`).
- Decision: `steam_heat_boiler` becomes a HEAT `consumer` (`machines.yaml`: `energy_in: HEAT`,
  `role: consumer`) so `AdjacencyTransferSystem` delivers neighbour heat into its `EnergyStorage`
  (type HEAT) and syncs `HeatIntakeComponent.heat_stored` — identical to `heat_furnace`. It then
  converts that heat → STEAM. No PipeNetwork heat-sink registration; reuse the existing adjacency path.
- Decision: the heat boiler's STEAM output cannot live in its HEAT `EnergyStorage`. Add a small
  `SteamOutputComponent { int32_t current; int32_t capacity; }` (mirroring `HeatIntakeComponent`,
  `HeatIntakeComponent.h`) that `BoilerSystem` fills and from which it publishes the STEAM source node.
  (Transient per-tick publishing without a buffer is the fallback if a component is undesirable.)
- Decision: steam output = existing `pipeClient_->publishNodeUpdate(...)` with `is_source=true` and
  `type=STEAM` — same call the current code already uses for STEAM/HEAT (`BoilerSystem.cpp:42`, `:80`).
  No new transport code.
- Decision: client dual bars need both values on the wire. Extend `BlockEntityUpdate`
  (FlatBuffers schema under `src/protocol/`) with `steam_current` / `steam_capacity`; the client
  renders the STEAM bar from those and the HEAT bar from `temperature` (heat ratio, already sent as
  `HeatIntakeComponent.ratio()`). The solid boiler's `temperature` will read ~0 (it has no external
  heat buffer); that is correct for the coal model.

## Risks / Trade-offs
- Double-publish / ownership: `GeneratorSystem` owns coal→STEAM for the solid boiler; `BoilerSystem`
  owns heat→STEAM for the heat boiler. The two machines are distinct IDs, so no shared-buffer conflict.
  For the heat boiler, ensure `BoilerSystem` deducts consumed heat from `heat_stored` so
  `AdjacencyTransferSystem` (which reads `energy.current`) and the steam conversion agree.
- New `SteamOutputComponent` must be attached to `1110:01:1` entities by the engine; verify the
  component-attachment path mirrors `HeatIntakeComponent`.
- Protocol change (`BlockEntityUpdate`) requires regenerating FlatBuffers headers and keeping the
  client/server in sync; add the fields with safe defaults so old readers ignore them.

## Migration Plan
- Existing placed boilers keep their slots; only behaviour changes (water no longer consumed, heat
  boiler inverted). No data migration.
- Rollback: revert `BoilerSystem.cpp`, `GeneratorSystem.cpp`, `machines.yaml`, `MachineWindow.cpp`,
  the new component, and the `BlockEntityUpdate` schema + generated headers.

## Open Questions
- None remaining. Solid boiler = coal combustion (existing `GeneratorSystem`); heat boiler = HEAT
  consumer via `AdjacencyTransferSystem` + STEAM output via `SteamOutputComponent`. Steam transport =
  existing PipeNetwork node publish.
