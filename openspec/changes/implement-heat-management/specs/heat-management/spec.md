## ADDED Requirements

### Requirement: Boiler Steam Conversion
The system SHALL convert water and heat into steam via boiler machines.

#### Scenario: Boiler produces steam from fuel
- **GIVEN** a boiler machine with fuel in firebox and water in input hatch
- **WHEN** the boiler tick runs
- **THEN** fuel is consumed, HEAT is consumed from EnergyStorage
- **AND** STEAM energy is produced and available for steam machines

#### Scenario: Boiler idle without water
- **GIVEN** a boiler with fuel but no water
- **WHEN** the boiler tick runs
- **THEN** heat builds up without steam production (overheat risk)

### Requirement: Coolant-Based Cooling
The system SHALL support coolant items for active heat reduction.

#### Scenario: Coolant absorbs heat
- **GIVEN** a machine with coolant in its inventory
- **WHEN** heat exceeds threshold
- **THEN** coolant is consumed to reduce heat by cooling_rate
- **AND** coolant depletes (full → empty bucket)

### Requirement: Pipe Heat Transport
The system SHALL transport heat through the pipe network to distant consumers.

#### Scenario: Heat flows through pipes
- **GIVEN** a HEAT producer connected to pipes leading to a consumer
- **WHEN** the pipe network ticks
- **THEN** heat is transferred from producer through pipes to consumer
- **AND** heat loss occurs per pipe block traversed

### Requirement: Overheat UI Warnings
The system SHALL display heat warnings in the machine UI.

#### Scenario: Warning at 90% heat
- **GIVEN** a machine with heat at 90%+ capacity
- **WHEN** the client renders the machine window
- **THEN** a yellow warning indicator is shown

#### Scenario: Critical at 100% heat
- **GIVEN** a machine with heat at 100% capacity
- **WHEN** the client renders the machine window
- **THEN** a red critical indicator is shown and processing stops
