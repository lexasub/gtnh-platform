# heat-management Specification

## Purpose
TBD - created by archiving change implement-heat-management. Update Purpose after archive.
## Requirements
### Requirement: 6-Neighbor Heat Propagation
Machines with `EnergyType::HEAT` SHALL propagate heat to adjacent machines (6-neighbor Manhattan) each tick.

#### Scenario: Producer transfers heat to adjacent consumer
- **GIVEN** a PRODUCER machine with `EnergyType::HEAT` and `energy.current > 0`
- **AND** a CONSUMER machine with `EnergyType::HEAT` and `energy.current < energy.capacity`
- **AND** the consumer is at (x±1, y, z), (x, y±1, z), or (x, y, z±1) from the producer
- **WHEN** `HeatTransferSystem::tick()` runs (Pass 1)
- **THEN** heat transfers from producer to consumer up to consumer's remaining capacity
- **AND** `HeatIntakeComponent.heat_stored` is synced to `EnergyStorage.current`

#### Scenario: No transfer when consumer full
- **GIVEN** a consumer with `energy.current >= energy.capacity`
- **WHEN** `HeatTransferSystem::tick()` runs
- **THEN** no heat transfer occurs to that consumer

### Requirement: Environment Cooling
The system SHALL passively cool machines with stored heat each tick.

#### Scenario: Base environment cooling
- **GIVEN** a machine with `HeatIntakeComponent.heat_stored > 0`
- **WHEN** `HeatTransferSystem::tick()` runs (Pass 3)
- **THEN** heat decreases by `HeatConstants::ENVIRONMENT_COOLING_RATE = 4.0`

#### Scenario: Water adjacency cooling bonus
- **GIVEN** a machine adjacent to a water block (ItemId `0:0:9`)
- **WHEN** environment cooling is applied
- **THEN** cooling rate is multiplied by `HeatConstants::WATER_COOLING_MULTIPLIER = 3.0`
- **AND** total cooling = `4.0 * 3.0 = 12.0` per tick

### Requirement: Overheat Detection
The system SHALL detect overheat states on multiblock machines based on heat ratio relative to capacity.

#### Scenario: Warning threshold at 90%
- **GIVEN** a multiblock machine with `HeatIntakeComponent.ratio() >= 0.90`
- **AND** `ratio() < 1.00`
- **WHEN** `HeatTransferSystem::tick()` runs (Pass 2)
- **THEN** `OverheatComponent.state` is set to `WARNING`

#### Scenario: Critical threshold at 100%
- **GIVEN** a multiblock machine with `HeatIntakeComponent.ratio() >= 1.00`
- **WHEN** `HeatTransferSystem::tick()` runs (Pass 2)
- **THEN** `OverheatComponent.state` is set to `CRITICAL`

#### Scenario: Recovery below warning threshold
- **GIVEN** a multiblock machine with `OverheatComponent` present
- **AND** `HeatIntakeComponent.ratio() < 0.90`
- **WHEN** `HeatTransferSystem::tick()` runs (Pass 2)
- **THEN** `OverheatComponent` is removed from the entity

### Requirement: Boiler Steam Production
A `steam_solid_boiler` machine SHALL convert water and heat into STEAM energy.

#### Scenario: Boiler produces steam from heat and water
- **GIVEN** a `steam_solid_boiler` machine (`1110:01:0`)
- **AND** `HeatIntakeComponent.heat_stored > 0`
- **AND** inventory slot 0 contains a water bucket (`0:11111:0`)
- **WHEN** `BoilerSystem::tick()` runs
- **THEN** 1 unit of heat is consumed from `HeatIntakeComponent`
- **AND** water bucket count decrements (becomes empty bucket `0:11111:3` when depleted)
- **AND** 1 unit of STEAM is produced into `EnergyStorage`
- **AND** a PipeNetwork node update is published via `PipeEnergyClient`

#### Scenario: Boiler idle without water (dry run)
- **GIVEN** a `steam_solid_boiler` with heat but no water bucket in slot 0
- **WHEN** `BoilerSystem::tick()` runs
- **THEN** no steam is produced
- **AND** heat builds up without consumption (overheat risk)

#### Scenario: Boiler idle with full energy storage
- **GIVEN** a `steam_solid_boiler` with `energy.isFull() == true`
- **WHEN** `BoilerSystem::tick()` runs
- **THEN** no water is consumed
- **AND** no steam is produced

### Requirement: Coolant-Based Active Cooling
The system SHALL support coolant items for active heat reduction during overheat on multiblock machines.

#### Scenario: Coolant reduces heat during overheat
- **GIVEN** a multiblock machine with `OverheatComponent.state == WARNING` or `CRITICAL`
- **AND** `HeatIntakeComponent.heat_stored > 0`
- **AND** coolant items (`HeatConstants::COOLANT_ITEM_ID`, resolved to a real registered coolant item) in inventory
- **WHEN** `CoolantSystem::tick()` runs
- **THEN** one coolant item is consumed (count decrements, slot cleared at 0)
- **AND** `heat_stored` is reduced by `HeatConstants::COOLANT_COOLING_AMOUNT = 50`
- **AND** `EnergyStorage.current` is synced

#### Scenario: Coolant not consumed when not overheated
- **GIVEN** a multiblock machine with coolant in inventory but no OverheatComponent
- **WHEN** `CoolantSystem::tick()` runs
- **THEN** coolant items are NOT consumed
- **AND** no heat reduction occurs

### Requirement: Pipe Heat Transport
The system SHALL transport heat through the pipe network graph via network-wide pooling.

#### Scenario: Heat flows from source to sink through pipes
- **GIVEN** pipe network nodes with `heatStored > heatCapacity * 0.9` (heat sources)
- **AND** pipe network nodes with `heatStored < heatCapacity` (heat sinks)
- **WHEN** `PipeNetworkManager::distributeHeat()` runs
- **THEN** excess heat (>90% capacity) flows from sources to sinks
- **AND** flow is capped at `HeatConstants::MAX_HEAT_PER_TICK = 1000`

#### Scenario: Heat loss and temperature computed by dedicated class
- **GIVEN** a heat source and sink connected by pipes with nonzero `resistance`
- **WHEN** heat is distributed
- **THEN** a dedicated `HeatLoss` module in `pipe_network/` (mirroring `CableLoss.h`/`CableOverheat.h`) computes the effective heat transfer reduced by traversed-edge resistance × distance
- **AND** per-node temperature is tracked by the same module

#### Scenario: No flow when no excess heat
- **GIVEN** all pipe nodes at or below 90% heat capacity
- **WHEN** `distributeHeat()` runs
- **THEN** `heatToFlow = 0` and no transfer occurs

### Requirement: Client Overheat UI Warnings
The system SHALL display heat warnings in the machine UI when viewing multiblock machines.

#### Scenario: Yellow warning bar at 90%+ heat
- **GIVEN** a multiblock machine (`mbId > 0`)
- **AND** `heatRatio >= 0.90` and `< 1.00`
- **WHEN** the client renders the machine window energy bar via `RenderEnergyBarImpl()`
- **THEN** the energy bar is rendered in yellow (`IM_COL32(255, 200, 0, 255)`)
- **Source**: `game_client/UI/Windows/block/MachineWindow.cpp:218-219`

#### Scenario: Red critical indicator at 100% heat
- **GIVEN** a multiblock machine (`mbId > 0`)
- **AND** `heatRatio >= 1.00`
- **WHEN** the client renders the machine window energy bar
- **THEN** the energy bar is rendered in red (`IM_COL32(255, 40, 40, 255)`)
- **Source**: `game_client/UI/Windows/block/MachineWindow.cpp:216-217`

#### Scenario: Non-multiblock gray bar regardless of heat
- **GIVEN** a machine with `mbId == 0`
- **WHEN** the energy bar is rendered
- **THEN** overheat color overrides are NOT applied (standard energy type color used)

### Requirement: Heat Network Registration
All heat-related ECS systems SHALL be registered in the SimulationEngine tick loop.

#### Scenario: Systems registered in main.cpp
- **GIVEN** `simulation_core/main.cpp` startup
- **WHEN** the service initializes
- **THEN** these systems MUST be registered:
  - `HeatTransferSystem` (line 213-215)
  - `BoilerSystem` (line 85)
  - `CoolantSystem` (line 81)
  - `ExplosionSystem` (line 82)
  - `GeneratorSystem` (line 83)
  - `CreativeGeneratorSystem` (line 84)
- **AND** `tickAll()` MUST call each system's `tick()` every cycle (line 310)

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

### Requirement: Boiler Heat and Steam UI Display
The machine UI (`MachineWindow`) for boiler machines SHALL display both a heat buffer bar and a steam buffer bar.

#### Scenario: Boiler shows heat and steam bars
- **GIVEN** a boiler machine (`1110:01:0` or `1110:01:1`) open in the machine window
- **WHEN** the window renders
- **THEN** a STEAM bar is shown from the steam output (`steam_current` / `steam_capacity`)
- **AND** a HEAT bar is shown from `HeatIntakeComponent` (`heat_stored` / `heat_capacity`)

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

