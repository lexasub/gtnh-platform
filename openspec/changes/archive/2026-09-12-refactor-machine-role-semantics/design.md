## Context
`MachineRole` was introduced as a coarse machine classification but is consumed **only** by
`AdjacencyTransferSystem` to decide heat-flow direction. Two orthogonal concepts are fused into one
flag: (a) energy production/consumption (`energy_in`/`energy_out`) and (b) heat-network topology
(who pushes/pulls HEAT). `MachineFaceRole` (per-face I/O in `ItemFlowHandler`/`FluidFlowHandler`) is
unrelated and untouched.

Current usages (verified by grep): `MachineRegistry.{h,cpp}` (enum, YAML parse, `IsConsumer`/
`IsProducer`), `AdjacencyTransferSystem.cpp:45,76`, `main.cpp`, `GameClient.cpp`,
`test_ecs_systems.cpp`. Notably `GeneratorSystem.cpp` ignores `role` entirely (it keys off
`EnergyStorage.type`). `PipeNetwork.*` does not reference `MachineRole`.

## Goals / Non-Goals
- Goals: make machine heat-network participation derivable and self-consistent; eliminate the
  misleading "producer/consumer" naming; remove the manual flag so boiler labels cannot disagree.
- Non-Goals: changing `MachineFaceRole`; reworking PipeNetwork pooling; altering steam/heat physics
  or boiler behaviour (that is `make-boiler-water-free`'s scope).

## Decisions
- **Primary: remove `MachineRole`, derive topology.** Source/sink determined by
  `EnergyStorage.type == HEAT` combined with `energy_in`/`energy_out`:
  - `IsHeatSource(block_id)` = storage type HEAT AND (`energy_out == HEAT` OR internal producer).
  - `IsHeatSink(block_id)`   = storage type HEAT AND `energy_in == HEAT`.
  `AdjacencyTransferSystem` already filters `EnergyStorage.type == HEAT` (lines 38, 72), so non-HEAT
  machines (`steam_solid_boiler`, STEAM storage) are excluded automatically — the manual flag is dead.
- **Fallback (lower blast radius): rename-only** `MachineRole` → `HeatNetworkRole { SOURCE, SINK }`
  if removing the field is unacceptable. This removes the misleading name but does NOT fix the
  inconsistent boiler labelling. Choose primary unless coordination risk is too high.
- Replace `IsConsumer`/`IsProducer` with `IsHeatSink`/`IsHeatSource` (or inline in the system).

## Risks / Trade-offs
- Removing the field changes the YAML schema and `MachineInfo` ABI → rebuild + `ctest` required.
- Other agents currently edit `SimulationEngine.cpp` (isInfraBlock refactor), `core.fbs`
  (ToolActionResp), and `PipeNetwork.*`; rebase/coordinate before the refactor lands.
- `main.cpp` / `GameClient.cpp` hardcode `MachineRole` for demo/test machines → must move to the
  derived model (registry loads them without a role).

## Migration Plan
1. Add derived `IsHeatSource` / `IsHeatSink` helpers in `MachineRegistry` (from `energy_in`/`energy_out`
   + `EnergyStorage.type`).
2. Switch `AdjacencyTransferSystem` to derived checks; delete the `//TODO fix read real info`.
3. Remove `MachineRole` enum, `role` member, YAML `role` parse, hardcoded role in `main`/`GameClient`.
4. Update `test_ecs_systems.cpp` role assertions → topology assertions.
5. Remove `role:` from `machines.yaml`.
6. Rebuild (`ninja simcored_test`) + `ctest` (green) + `openspec validate --strict`.
7. Rollback: revert the single commit; no data migration (registry is rebuilt from YAML each run).

## Open Questions
- Keep an optional explicit topology override in YAML for machines whose participation cannot be
  derived? (Defer — not needed for the current machine set.)
- Should any `energy_out == HEAT` machine act as a heat SOURCE uniformly (instead of only
  `heat_generator`)? (Adopt the uniform rule.)
