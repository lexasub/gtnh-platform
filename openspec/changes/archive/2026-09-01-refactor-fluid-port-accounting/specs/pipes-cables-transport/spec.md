## MODIFIED Requirements

### Requirement: Fluid Pipe Transport

The system SHALL support fluid transport between machines via fluid pipes, with
fluid type tracking, per-fluid connected components, and volume distribution.
Each machine resource port SHALL be registered independently, so a converter may
be both a source and a sink in different resource domains. Resource channels are
`FLUID`, `EU`, `HU`, `RU`, and `ITEM`; Steam is a `FLUID`, not a separate channel.
Ports have no resource filters in this change. The concrete resource identity for
FLUID and ITEM is always the canonical packed ID from `items.csv`. A fluid pipe
network MUST never combine different fluid item IDs, while an ITEM output port
may emit different concrete item IDs over time.

#### Scenario: Fluid pipe block placed
- **GIVEN** no fluid pipe network exists
- **WHEN** a player places a registered `fluid_pipe` adjacent to an existing pipe
  or compatible fluid port
- **THEN** the network manager creates a fluid node using the canonical item ID
  from `items.csv`
- **AND** the node has its configured capacity and an empty fluid buffer

#### Scenario: Fluid flows through pipe network
- **GIVEN** a fluid pipe network connects a source port and a compatible sink port
- **WHEN** the source owner accepts a `DrainRequest`
- **THEN** exactly the accepted amount enters PipeNetwork-owned pipe buffers
- **AND** distribution is limited by the source rate, pipe capacity, sink demand,
  and canonical `fluid_id`
- **AND** any flow event is telemetry and cannot mutate an owner buffer

#### Scenario: Fluid inserted into destination machine
- **GIVEN** a fluid pipe network has fluid available for a compatible sink port
- **WHEN** the sink sends a `ConsumeRequest`
- **THEN** PipeNetwork drains its pipe buffers first
- **AND** requests any shortfall from compatible source owners
- **AND** `ConsumeResponse.accepted_amount` reports the exact amount accepted by
  the sink
- **AND** the sink machine buffer increases only once from that response

#### Scenario: Fluid type mismatch blocks flow
- **GIVEN** a fluid pipe network contains `fluid_id = 1` (water)
- **WHEN** a source or sink with `fluid_id = 2` (steam) requests transport on the
  same network
- **THEN** the mismatched amount is not accepted or mixed
- **AND** the caller receives a blocked or short-fill response
- **AND** the existing fluid buffer remains unchanged

#### Scenario: Fluid requests are replay-safe
- **GIVEN** a source drain or sink consume request with `request_id = R`
- **WHEN** the same request is delivered more than once
- **THEN** the owner returns the cached response for `R`
- **AND** the resource is debited or credited only once

#### Scenario: Fluid port removal
- **GIVEN** a machine has one or more registered resource ports
- **WHEN** the machine is removed or its block is replaced
- **THEN** all ports owned by that machine are unregistered
- **AND** their pending requests are completed as zero/short-fill
- **AND** no future flow targets the removed machine
