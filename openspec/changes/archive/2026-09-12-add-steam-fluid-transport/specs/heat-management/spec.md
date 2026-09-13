## MODIFIED Requirements
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
