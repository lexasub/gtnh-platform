## ADDED Requirements

### Requirement: Headless thermal power chain
The platform SHALL provide a deterministic headless test path for multiple heat sources, heat-to-steam boilers, steam-to-electric turbines, electricity buffers, and a 32-EU/t electric machine.

#### Scenario: Chain reaches observable state transitions
- **WHEN** the test places registered source, converter, transport, buffer, and consumer blocks and inserts their required items
- **THEN** correlated acknowledgements are accepted and observable machine state shows heat, steam, EU, rechargeable-item metadata, and consumer progress transitions

### Requirement: Separate turbine resource domains
Steam turbine input and EU output SHALL be represented as independent bounded buffers and SHALL never debit or credit the wrong resource domain.

#### Scenario: Partial steam delivery
- **WHEN** a turbine receives less steam than requested
- **THEN** only the accepted steam amount is credited and EU production is limited to available steam and output capacity
