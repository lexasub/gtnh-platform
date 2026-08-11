# Change: Make boilers water-free steam producers

## Why
Boilers currently either require a water bucket (`steam_solid_boiler`) or run backwards as a
STEAM→HEAT converter (`steam_heat_boiler`). Water supply is not yet a real constraint, and the
`steam_heat_boiler` was inverted by mistake. We make both boilers emit STEAM, driven by different
inputs: the **solid boiler burns coal** (internal combustion), the **heat boiler consumes
externally-supplied HEAT** from a neighbouring heat machine. Water is not required for either.

## What Changes
- `steam_solid_boiler` (`1110:01:0`): produces STEAM from coal combustion via `GeneratorSystem`.
  Remove the dead water-bucket gate. Slots preserved (coal stays in input slot 0).
- `steam_heat_boiler` (`1110:01:1`): reversed STEAM→HEAT into HEAT→STEAM. It becomes a HEAT
  `consumer` (adjacent `heat_generator` / `heat_furnace`) and a STEAM producer. Slots preserved.
- Neither boiler consumes any water item.
- Steam output is published as a PipeNetwork source node (existing `publishNodeUpdate`). No
  back-face transfer for MVP.
- Boiler UI shows both a HEAT buffer bar and a STEAM buffer bar.

## Impact
- Affected specs: `heat-management` (MODIFIED boiler requirements; ADDED UI requirement)
- Affected code:
  - `src/services/simulation_core/ECS/Systems/BoilerSystem.cpp` (delete dead solid branch; rewrite heat branch)
  - `src/services/simulation_core/ECS/Systems/GeneratorSystem.cpp` (publish STEAM node for the solid boiler)
  - `src/services/simulation_core/ECS/components/` (new `SteamOutputComponent` for the heat boiler)
  - `data/registry/machines.yaml` (re-role `1110:01:1` as HEAT consumer)
  - `src/services/game_client/UI/Windows/block/MachineWindow.cpp` (dual heat/steam bars)
  - `src/protocol/*.fbs` `BlockEntityUpdate` (add `steam_current` / `steam_capacity`)
  - `src/services/simulation_core/ECS/test/test_ecs_systems.cpp` (boiler tests)
- Non-goals: building a `heat_generator` machine; back-face steam push to non-pipe neighbours;
  finite-water / steam-balance mechanics.
