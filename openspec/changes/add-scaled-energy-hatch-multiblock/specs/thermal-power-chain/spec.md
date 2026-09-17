## ADDED Requirements

### Requirement: Physical energy hatch endpoint
The platform SHALL expose a canonical LV energy hatch as the electricity endpoint of an LCR multiblock while retaining the LCR controller as the ECS owner.

#### Scenario: LCR energy hatch receives EU
- **GIVEN** an LCR pattern is formed with `hatch_energy_input_lv` at its declared ENERGY position
- **WHEN** an LV cable network supplies EU
- **THEN** the LCR energy node SHALL be advertised at the physical hatch coordinates
- **AND** accepted EU SHALL increase the controller-owned LCR energy buffer
- **AND** an absent hatch SHALL not advertise a physical hatch endpoint

### Requirement: Scaled thermal multiblock path
The platform SHALL provide a deterministic headless path with multiple thermal producers, steam turbines, battery buffers, and an LCR energy hatch.

#### Scenario: Multiple branches feed one LCR
- **GIVEN** multiple fuelled heat generators, heat boilers, fluid-pipe branches, steam turbines, LV battery buffers, and an LCR are placed through Gateway
- **WHEN** canonical hatch inputs and recipe items are inserted
- **THEN** every placement and slot operation SHALL receive a correlated accepted response
- **AND** each branch SHALL expose positive steam/EU or battery state
- **AND** the formed LCR SHALL expose recipe progress driven through its energy hatch

### Requirement: Correlated legacy energy responses
The platform SHALL correlate legacy energy consume responses to the requesting node.

#### Scenario: Multiple battery sinks and one multiblock sink
- **GIVEN** several battery buffers and an LCR issue simultaneous energy consume requests
- **WHEN** PipeNetwork responds to those requests
- **THEN** each response SHALL carry the requesting node identity
- **AND** only the matching owner SHALL credit the accepted amount
- **AND** a duplicate or late response SHALL not credit another owner
