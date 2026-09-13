# Change: Clarify and refactor machine role / heat-network topology semantics

## Why
The `MachineRole { CONSUMER, PRODUCER }` field is named as if it means "does the machine
produce/consume energy", but in `AdjacencyTransferSystem` it actually means "heat-network
source/sink". That misnaming produced a real inconsistency: the two steam-producing boilers carry
opposite `role` labels — `steam_solid_boiler` (coal, `energy_out: STEAM`) is `producer`, while
`steam_heat_boiler` (`energy_in: HEAT, energy_out: STEAM`) MUST be `consumer` so it can receive
neighbour HEAT. Both produce STEAM, yet their labels differ, and the label says nothing about steam.
There is even a `//TODO fix read real info` at `AdjacencyTransferSystem.cpp:45` acknowledging the hack.
The committed `make-boiler-water-free` change already corrects boiler behaviour and its own spec
delta; this change adds an explicit role-semantics requirement and removes the manual flag at the root.

## What Changes
- **Document** the precise meaning of `MachineRole` in the `heat-management` spec: PRODUCER = heat
  SOURCE, CONSUMER = heat SINK; independent of `energy_in`/`energy_out`; STEAM output is handled
  separately by `BoilerSystem`/`GeneratorSystem` via `energy_out`.
- **Refactor (BREAKING)**: remove the manual `MachineRole` flag and derive a machine's heat-network
  participation from `energy_in` / `energy_out` + its runtime `EnergyStorage.type`:
  - HEAT SOURCE = `EnergyStorage.type == HEAT` AND (`energy_out == HEAT` OR generator-style internal
    production).
  - HEAT SINK = `EnergyStorage.type == HEAT` AND `energy_in == HEAT`.
  - STEAM-only / non-HEAT machines are naturally excluded (their storage type is not HEAT), so the
    spurious `role: producer` on `steam_solid_boiler` becomes dead/no-longer-needed.
- Update all `role` accessors/usages: `MachineRegistry` (`IsConsumer`/`IsProducer`, YAML parse),
  `AdjacencyTransferSystem` (lines 45, 76) — replace role checks with derived source/sink logic,
  `main.cpp` and `GameClient.cpp` hardcoded registrations, and `test_ecs_systems.cpp`.
- Drop the `role:` field from `machines.yaml` (design decides override policy).
- Coordinate with in-flight agents editing `SimulationEngine.cpp`, `core.fbs`, `PipeNetwork.*`
  before merging the code refactor.

## Impact
- Affected specs: `heat-management` (ADDED role-semantics requirement; boiler requirements'
  `MachineRole` references purged as part of refactor implementation — those corrections are
  primarily owned by `make-boiler-water-free` and this change coordinates with it).
- Affected code:
  - `src/libs/machine_registry/MachineRegistry.h` / `.cpp` (remove enum/role, add derived helpers)
  - `src/services/simulation_core/ECS/Systems/AdjacencyTransferSystem.cpp` (derive source/sink)
  - `src/services/simulation_core/main.cpp`, `src/services/game_client/GameClient.cpp`
    (remove hardcoded `MachineRole`)
  - `src/services/simulation_core/test/test_ecs_systems.cpp` (update role assertions)
  - `data/registry/machines.yaml` (drop `role:`)
- Risk: BREAKING change to the machine registry ABI and YAML schema; requires rebuild + test pass.
- Coordination: `make-boiler-water-free` is still pending archive and references `MachineRole` in its
  boiler deltas — archive it first; this change's implementation then removes `MachineRole` and
  updates those references.
