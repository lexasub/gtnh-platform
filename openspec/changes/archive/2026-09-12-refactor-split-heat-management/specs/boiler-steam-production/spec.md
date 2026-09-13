## ADDED Requirements

### Requirement: Boiler Steam Production
A `steam_solid_boiler` machine SHALL convert water and heat into STEAM energy, and the produced steam SHALL be exported to the fluid pipe network as a fluid source (fluid_id = steam `1111:11:1`) so steam machines can consume it through fluid pipes.

**References:**
- `src/services/simulation_core/ECS/Systems/GeneratorSystem.cpp` — solid boiler STEAM branch, publishes `fluid.node.update` (steam, is_source = true)
- `src/services/simulation_core/ECS/Systems/BoilerSystem.cpp` — heat boiler, publishes `fluid.node.update` (steam, is_source = true)
- `src/services/simulation_core/ECS/Reactors/FluidFlowHandler.cpp` — Case 2 (STEAM `EnergyStorage`) / Case 2b (`SteamOutputComponent`) drain
- `src/services/simulation_core/ECS/components/SteamOutputComponent.h` — heat boiler steam pool

#### Scenario: Boiler steam available to fluid pipes
- **GIVEN** a boiler with stored steam (`energy.current` for `1110:01:0`, `steam_stored` for `1110:01:1`)
- **WHEN** the boiler's system publishes node updates
- **THEN** both `energy.node.update` and `fluid.node.update` (steam, is_source = true) are published
- **AND** both updates carry the same amount so the shared node state stays consistent

#### Scenario: Boiler steam drained via fluid consumption
- **GIVEN** a boiler whose steam is being consumed through a fluid pipe
- **WHEN** `FluidFlowHandler` receives the `FluidFlowEvent`
- **THEN** the boiler's steam pool is reduced (`energy.current` for `1110:01:0`, `steam_stored` for `1110:01:1`)
- **AND** subsequent node updates reflect the reduced amount
### Requirement: Boiler Heat-to-Steam Conversion
A `steam_heat_boiler` machine (`1110:01:1`) SHALL convert externally-supplied HEAT into STEAM energy. Water is NOT required; inventory slots SHALL be preserved. The machine SHALL act as a HEAT consumer (an `energy_in` of `HEAT`) and receive heat from an adjacent heat-producing machine via `AdjacencyTransferSystem`; it SHALL act as a STEAM producer.

#### Scenario: Converter produces steam from external heat
- **GIVEN** a `steam_heat_boiler` (`1110:01:1`) placed adjacent to a `heat_generator`
- **AND** HEAT has been delivered to it (`HeatIntakeComponent.heat_stored > 0` via `AdjacencyTransferSystem`)
- **AND** its steam output buffer is not full
- **WHEN** `BoilerSystem::tick()` runs
- **THEN** HEAT is consumed from `heat_stored`
- **AND** an equivalent amount of STEAM is produced into its steam output buffer
- **AND** a PipeNetwork STEAM source node update is published via `PipeEnergyClient`

#### Scenario: Converter idle without heat
- **GIVEN** a `steam_heat_boiler` with no heat available (`heat_stored == 0` and no adjacent heat source)
- **WHEN** `BoilerSystem::tick()` runs
- **THEN** no steam is produced
### Requirement: Boiler Heat and Steam UI Display
The machine UI (`MachineWindow`) for boiler machines SHALL display both a heat buffer bar and a steam buffer bar.

#### Scenario: Boiler shows heat and steam bars
- **GIVEN** a boiler machine (`1110:01:0` or `1110:01:1`) open in the machine window
- **WHEN** the window renders
- **THEN** a STEAM bar is shown from the steam output (`steam_current` / `steam_capacity`)
- **AND** a HEAT bar is shown from `HeatIntakeComponent` (`heat_stored` / `heat_capacity`)
