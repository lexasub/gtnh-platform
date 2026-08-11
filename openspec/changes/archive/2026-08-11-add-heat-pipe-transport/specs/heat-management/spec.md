## ADDED Requirements

### Requirement: Heat Pipe Block
The platform SHALL provide a `heat_pipe` block (`1111:10:4`) that transports HEAT through the pipe network. Its pipe node SHALL have `heatCapacity = 1000`.

#### Scenario: Heat pipe node carries heat capacity
- **GIVEN** a `heat_pipe` block (`1111:10:4`) placed in the world
- **WHEN** `PipeNetworkManager::addNode()` registers it
- **THEN** its `PipeNode.heatCapacity` equals 1000
- **AND** its `itemCapacity` and `fluidCapacity` are 0

#### Scenario: Heat pipe connects to HEAT machine nodes
- **GIVEN** a `heat_pipe` adjacent to a machine whose registered energy node has `EnergyType == HEAT` (e.g. `heat_generator` output or `steam_heat_boiler` input)
- **WHEN** connectivity is (re)built via `connectNodeNeighbors()`
- **THEN** an edge is created between the heat pipe and the machine node
- **AND** a machine with `EnergyType == STEAM` only is NOT linked by a heat pipe

### Requirement: Pipe Renderer Machine Connections
The game client renderer SHALL draw a pipe/cable connection (flange + tube) to any adjacent machine block, not only to same-type pipes/cables.

#### Scenario: Pipe stub against a machine renders connected
- **GIVEN** a `fluid_pipe` block with a machine block (e.g. `steam_heat_boiler`, `1110:01:1`) on one face
- **WHEN** `PipeMeshBuilder::detectConnections()` runs for that pipe
- **THEN** the face mask includes the face toward the machine
- **AND** the mesh builder emits a tube/flange for that face

#### Scenario: Cable against a machine renders connected
- **GIVEN** a cable block with a machine block on one face
- **WHEN** `CableMeshBuilder::detectConnections()` runs
- **THEN** the face mask includes the face toward the machine

#### Scenario: Same-type pipe connection is preserved
- **GIVEN** a pipe with a same-type pipe neighbour
- **WHEN** `detectConnections()` runs
- **THEN** the face mask still includes that face

## MODIFIED Requirements

### Requirement: Boiler Heat-to-Steam Conversion
A `steam_heat_boiler` machine (`1110:01:1`) SHALL convert externally-supplied HEAT into STEAM energy. Water is NOT required; inventory slots SHALL be preserved. The machine SHALL act as a HEAT consumer (`MachineRole::CONSUMER`, `EnergyType::HEAT`) and receive heat from an adjacent heat-producing machine via `AdjacencyTransferSystem` **or from a heat pipe network via its HEAT sink node**; it SHALL act as a STEAM producer.

#### Scenario: Converter produces steam from pipe-delivered heat
- **GIVEN** a `steam_heat_boiler` (`1110:01:1`) connected to a `heat_pipe` network whose sources have excess heat
- **AND** `HeatIntakeComponent.heat_stored` is below the replenish threshold
- **AND** its steam output buffer is not full
- **WHEN** `BoilerSystem::tick()` runs
- **THEN** a HEAT sink node update is published (`is_sink=true`)
- **AND** an `EnergyConsumeReq` (HEAT) is sent for the deficit
- **AND** heat delivered by the network lands in `HeatIntakeComponent.heat_stored` (synced to `EnergyStorage.current`)
- **AND** subsequent ticks convert HEAT into STEAM as before

#### Scenario: Converter idle without heat
- **GIVEN** a `steam_heat_boiler` with no heat available (`heat_stored == 0` and no adjacent heat source)
- **WHEN** `BoilerSystem::tick()` runs
- **THEN** no steam is produced
