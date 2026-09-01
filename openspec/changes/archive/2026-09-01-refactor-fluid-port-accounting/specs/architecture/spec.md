## ADDED Requirements

### Requirement: Canonical Cross-Registry Item Identity

`data/registry/items.csv` SHALL be the sole canonical catalog and ID namespace
for every item, block, pipe, cable, and fluid. Every specialized registry row
is keyed by an `item_id` that MUST resolve to exactly one row in `items.csv`:
`pipes.csv.item_id`, `cables.csv.item_id`, and `fluids.csv.item_id`; a
`drops.csv` source and result also resolve there. Specialized CSVs contain only
properties and MUST NOT define a second numeric ID for the same object.

There is no separate `fluid_id`, `pipe_id`, or `cable_id`. Steam is the packed
`ItemId::pack("1111:11:1")` from `items.csv`; transport uses that same value in
fluid operations, while `ResourceKind::FLUID` identifies the channel. Pipe and
cable nodes likewise use their packed `items.csv` ID. Code MUST NOT substitute a
row number, array index, or legacy small integer for a canonical item ID.

Resource ports SHALL use only these channel kinds: `FLUID`, `EU`, `HU`, `RU`,
and `ITEM`. A channel kind SHALL NOT encode a particular fluid or item. Ports
SHALL NOT have resource filters in this change: concrete transfer requests carry
`fluid_id` for FLUID or `item_id` for ITEM, and the network enforces the
applicable channel rules. An ITEM output port may therefore emit different
concrete `item_id` values over time; no wildcard, allow-list, or exact-item
selector is introduced.

#### Scenario: Pipe and cable references resolve to items
- **GIVEN** a row in `pipes.csv` or `cables.csv`
- **WHEN** the shared registry loads the row
- **THEN** its `item_id` resolves through `items.csv`
- **AND** the referenced item name matches the property row name
- **AND** the canonical packed ID is used by simulation, transport, and client code

#### Scenario: Drop references resolve to items
- **GIVEN** a row in `drops.csv`
- **WHEN** the shared registry validates the row
- **THEN** both the source block and result item resolve through `items.csv`
- **AND** an unknown or duplicate reference is a hard validation error

#### Scenario: Fluid has separate resource and item identities
- **GIVEN** the Steam row in `fluids.csv`
- **WHEN** a transport message is built
- **THEN** it uses the canonical `fluid_id` from `fluids.csv`
- **AND** any bucket/item representation uses the row's `item_id` from `items.csv`
- **AND** no packed item ID is used as a fluid ID by accident

#### Scenario: Registry validation fails before serving
- **GIVEN** duplicate canonical IDs, missing references, duplicate names, or
  mismatched names across registry files
- **WHEN** a service starts
- **THEN** registry validation reports a hard error
- **AND** the service does not accept world simulation traffic

### Requirement: Resource Data Ownership

Each mutable resource amount SHALL have exactly one authoritative owner. SimulationCore
owns machine buffers; PipeNetwork owns fluid physically buffered in pipe nodes;
no service SHALL mirror and independently debit the same amount.

#### Scenario: Machine source is drained once
- **GIVEN** a boiler owns a Steam buffer and PipeNetwork needs Steam
- **WHEN** PipeNetwork requests a drain
- **THEN** SimulationCore validates and debits its buffer once
- **AND** returns the accepted amount
- **AND** a repeated request with the same request ID returns the original result
- **AND** no telemetry event performs a second debit

### Requirement: Server-Authoritative Resource State

The server SHALL publish typed machine/port resource state through Gateway. The
client SHALL apply validated state on its state/render boundary and SHALL NOT
read raw PipeNetwork protocol messages in the render loop, invent coordinate-key
encodings, or hardcode resource labels.

#### Scenario: Client renders a resource buffer
- **GIVEN** a server `ResourceBufferState` for a machine port
- **WHEN** the client receives the update
- **THEN** the update is applied through the client state store
- **AND** the label and units are resolved from the registry
- **AND** the render loop displays the stored state without querying PipeNetwork
