## ADDED Requirements

### Requirement: Heat Network Topology Derived from Energy IO
Heat-network topology SHALL be derived from each machine's declared energy IO
(`energy_in` / `energy_out`) rather than a manual `MachineRole` flag.

A machine is a **heat source** iff its `energy_out` is `HEAT`.
A machine is a **heat sink** iff its `energy_in` is `HEAT`.

This covers pure heat machines (e.g. `heat_generator`, `1110:00:2`) and
converters (e.g. `steam_heat_boiler`, `1110:01:1`, which declares
`energy_in: HEAT` and `energy_out: STEAM` — it is a heat sink even though its
`EnergyStorage.type` resolves to `STEAM`).

#### Scenario: Heat source emits into the network
- **GIVEN** a machine with `energy_out: HEAT` (e.g. `heat_generator`)
- **WHEN** `AdjacencyTransferSystem::tick()` runs
- **THEN** it is treated as a heat source that can deliver heat to adjacent heat sinks

#### Scenario: Heat sink receives from the network
- **GIVEN** a machine with `energy_in: HEAT` (e.g. `steam_heat_boiler`)
- **WHEN** `AdjacencyTransferSystem::tick()` runs
- **THEN** it is treated as a heat sink that receives heat from adjacent heat sources

#### Scenario: Non-heat machines are excluded
- **GIVEN** a machine whose `energy_in`/`energy_out` are not `HEAT` (e.g. ELECTRICITY / STEAM / ROTATION)
- **WHEN** `AdjacencyTransferSystem::tick()` runs
- **THEN** it participates in neither the heat-source nor heat-sink pass

## MODIFIED Requirements

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
