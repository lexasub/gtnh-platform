# Change: Refactor typed resource ports and fluid accounting

## Why

The current boiler → fluid pipe → steam machine path uses one `PipeNode` and one
`NodeState` for several resource domains. A heat boiler publishes both a HEAT sink
and a STEAM source through the same identity, while machine buffers are mirrored
into PipeNetwork and debited again through `fluid.flow`. This makes message order,
entity-id collisions, and partial delivery affect correctness and can create or
lose steam.

The registry layer also has several incompatible ID domains: `items.csv` contains
all item/block/fluid-item IDs, while `pipes.csv`, `cables.csv`, and `fluids.csv`
currently mix canonical item references with independent small integer IDs, and
code duplicates fluid IDs. The transport and recipe paths need one explicit
identity model before more resources (electricity, water, gases) are added.

## What Changes

- Add typed resource ports with independent identities and per-domain roles; a
  machine may expose multiple ports, such as an HU sink and a FLUID source.
  Ports do not introduce resource filters in this change; concrete FLUID/ITEM
  operations carry their actual resource IDs.
- Make `PipeNetwork` authoritative only for fluid stored in pipe nodes. Machine
  buffers remain owned by SimulationCore. Replace mirror-debit flow events with
  request/response transactions carrying request IDs and accepted amounts.
- Make fluid source/sink registration, drain, short-fill, timeout, and replay
  behavior explicit and idempotent.
- Use one canonical ID namespace: `items.csv` is the catalog of every
  item/block/pipe/cable/fluid ID; `pipes.csv`, `cables.csv`, `fluids.csv`, and
  `drops.csv` contain properties and references to those IDs, never a second
  `fluid_id`, `pipe_id`, or `cable_id` for the same object.
- Start recipes only after the required resource reservation/consumption is
  accepted. Reuse the same resource contract for STEAM and future electricity.
- Deliver server-authoritative machine/resource state to the client through the
  existing gateway path; the client does not decode transport protocol messages
  in the render loop or hardcode fluid names.
- Keep topology as a graph with directional edge/port policy as a later
  extension; do not create one graph per direction in this change.

## Impact

- Affected specs: `architecture`, `pipes-cables-transport`, `recipe-id-format`.
- Affected code: shared registry library, `src/protocol/pipe_network.fbs`,
  PipeNetwork/PipeNetworkService, SimulationCore resource and machine systems,
  RecipeManager integration, Gateway/client state handling, and tests.
- **BREAKING**: fluid/node protocol messages and internal registration APIs gain
  typed port and transaction identity; old mirror-based flow handling must be
  migrated together.
- Out of scope: water production recipes, complete electricity transport,
  directional pipe gameplay/UI, and unrelated wrench/UI or CLI changes.
