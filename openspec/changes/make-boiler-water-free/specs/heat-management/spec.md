## MODIFIED Requirements

### Requirement: Boiler Steam Production
A `steam_solid_boiler` machine (`1110:01:0`) SHALL produce STEAM energy from coal combustion (internal fuel burn via `GeneratorSystem`). Water is NOT required: the machine SHALL NOT consume any water item, and inventory slots SHALL be preserved (no slot removal).

#### Scenario: Boiler produces steam from coal without water
- **GIVEN** a `steam_solid_boiler` (`1110:01:0`) with coal in an input slot
- **AND** `energy.isFull() == false`
- **WHEN** `GeneratorSystem::tick()` burns coal and the steam publish runs
- **THEN** STEAM is produced into `EnergyStorage` (type STEAM)
- **AND** no water item is consumed from any slot
- **AND** a PipeNetwork STEAM source node update is published via `PipeEnergyClient`

#### Scenario: Boiler idle without fuel
- **GIVEN** a `steam_solid_boiler` with no fuel in any input slot
- **WHEN** `GeneratorSystem::tick()` runs
- **THEN** no steam is produced

#### Scenario: Boiler idle with full steam storage
- **GIVEN** a `steam_solid_boiler` with `energy.isFull() == true`
- **WHEN** the tick runs
- **THEN** no steam is produced

## RENAMED Requirements
- FROM: `### Requirement: Boiler Steam-to-Heat Conversion`
- TO: `### Requirement: Boiler Heat-to-Steam Conversion`

## MODIFIED Requirements

### Requirement: Boiler Heat-to-Steam Conversion
A `steam_heat_boiler` machine (`1110:01:1`) SHALL convert externally-supplied HEAT into STEAM energy. Water is NOT required; inventory slots SHALL be preserved. The machine SHALL act as a HEAT consumer (`MachineRole::CONSUMER`, `EnergyType::HEAT`) and receive heat from an adjacent heat-producing machine via `AdjacencyTransferSystem`; it SHALL act as a STEAM producer.

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

## ADDED Requirements

### Requirement: Boiler Heat and Steam UI Display
The machine UI (`MachineWindow`) for boiler machines SHALL display both a heat buffer bar and a steam buffer bar.

#### Scenario: Boiler shows heat and steam bars
- **GIVEN** a boiler machine (`1110:01:0` or `1110:01:1`) open in the machine window
- **WHEN** the window renders
- **THEN** a STEAM bar is shown from the steam output (`steam_current` / `steam_capacity`)
- **AND** a HEAT bar is shown from `HeatIntakeComponent` (`heat_stored` / `heat_capacity`)
