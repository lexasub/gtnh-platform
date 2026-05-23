## ADDED Requirements

### Requirement: Item Pipe Transport
The system SHALL support item transport between machines via item pipes.

#### Scenario: Item moves from machine to pipe
- **GIVEN** a machine with OUTPUT role on a face connected to an item pipe
- **WHEN** the machine finishes a recipe
- **THEN** the output item is pushed into the connected pipe

#### Scenario: Item moves along pipe network
- **GIVEN** an item pipe network connects two machines
- **WHEN** an item enters the network
- **THEN** it moves 1 block per tick toward the destination
- **AND** reaches a machine with INPUT role on the connected face

### Requirement: Fluid Pipe Transport
The system SHALL support fluid transport between machines via fluid pipes.

#### Scenario: Fluid flows through fluid pipe network
- **GIVEN** a fluid pipe network connects machines with FLUID_OUTPUT and FLUID_INPUT roles
- **WHEN** a machine outputs fluid
- **THEN** the fluid flows through the network
- **AND** is inserted into the destination machine

### Requirement: Energy Cable Tiers
The system SHALL enforce voltage tiers for energy cables.

#### Scenario: Correct tier allows energy flow
- **GIVEN** a cable of tier T connects a generator to a machine
- **WHEN** the generator outputs voltage ≤ T
- **THEN** energy flows normally

#### Scenario: Overvoltage causes cable damage
- **GIVEN** a cable of tier T
- **WHEN** voltage exceeds tier rating
- **THEN** the cable overheats over time
- **AND** explodes if critical temperature is reached
