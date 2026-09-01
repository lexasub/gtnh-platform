# Design: Typed resource ports and single-owner accounting

## Context

SimulationCore owns machine ECS state and PipeNetwork solves transport graphs in a
separate process. A machine can be a converter: `steam_heat_boiler` consumes HEAT
and produces STEAM. Therefore resource type and port role cannot be represented
by one node-level `is_source`/`is_sink` pair. Machine entity IDs also cannot be
used as transport IDs because EnTT IDs and auto-assigned pipe IDs can collide.

## Goals / Non-Goals

- Goals: preserve every resource unit exactly once; support multiple typed ports
  per machine; handle partial delivery, retries, removal, and reconnect; keep
  recipes server-authoritative; align all registry references with `items.csv`.
- Non-goals: implement water generation, redesign the complete cable solver,
  add directional pipe gameplay, or expose a direct client-to-PipeNetwork RPC.

## Decisions

### 1. Canonical registry identity

`items.csv` is the sole canonical catalog and ID namespace for every item,
block, pipe, cable, and fluid. Every specialized registry row is keyed by an
`item_id` that MUST resolve to exactly one row in `items.csv`:
`pipes.csv.item_id`, `cables.csv.item_id`, and `fluids.csv.item_id`; a
`drops.csv` source and result also resolve there. The specialized CSVs contain
only properties; they do not define a second numeric ID for the same object.

There is no separate `fluid_id`, `pipe_id`, or `cable_id`. Steam, for example,
is the packed `ItemId::pack("1111:11:1")` from `items.csv`; transport uses that
same value in fluid operations, while `ResourceKind::FLUID` identifies the
channel. Pipe and cable nodes likewise use their packed `items.csv` ID. Code
must never substitute a row number, array index, or legacy small integer for a
canonical item ID.

A shared C++ registry library loads and validates these CSVs. Startup fails (or
reports a hard validation error before serving) for duplicate canonical IDs,
missing references, duplicate names, or a mismatch between a property row's
name and its referenced `items.csv` name.

A shared C++ registry library loads and validates these CSVs. Startup fails (or
reports a hard validation error before serving) for duplicate canonical IDs,
missing references, duplicate names, or a mismatch between a property row's
name and its referenced `items.csv` name.

### 2. Typed resource ports

Introduce a transport-facing `PortId` allocated by the owning service and a
`ResourcePort` record:

```text
PortId, owner machine instance, ResourceKind,
role (SOURCE/SINK), position, face policy,
capacity, rate, generation/epoch
```

`ResourceKind` describes the transport channel, not a particular material:
`FLUID`, `EU`, `HU`, `RU`, and `ITEM`. Steam is `FLUID` with `fluid_id=2`;
heat is `HU`; electricity is `EU`; rotation is `RU`.

A port does not carry a resource filter in this change. The port says only which
channel and direction it supports. The concrete operation message carries the
resource identity where the channel needs one: `fluid_id` for FLUID and
`item_id` for ITEM. Fluid networks must remain homogeneous by `fluid_id`; item
ports may emit different item IDs over time, so no wildcard, allow-list, or
exact-item selector is introduced. Energy channels do not need a resource ID.

A converter registers separate ports by channel and role, so the heat boiler has
an `HU` sink and a `FLUID` source. `PipeNode` stores only one resource domain;
its source/sink flags are never reused by another domain.

Port registration is idempotent by `(owner, port kind, port slot, epoch)` and
removal unregisters every port and pending request owned by the machine.

### 3. Single owner of amounts

SimulationCore owns machine buffers (`SteamOutputComponent`, `FluidStorage`,
heat storage, and machine energy buffers). PipeNetwork owns only fluid physically
buffered in pipe nodes. A node update advertises capacity, role, and a snapshot;
it does not transfer ownership or overwrite a machine amount every tick.

A resource crossing the service boundary is a transaction:

```text
DrainRequest(request_id, source_port_id, resource_id, amount)
DrainResponse(request_id, source_port_id, accepted_amount)
ConsumeRequest(request_id, sink_port_id, resource_id, amount)
ConsumeResponse(request_id, sink_port_id, accepted_amount, remaining)
```

The owner validates the port, resource ID, capacity, and available amount, then
returns `accepted_amount <= amount`. The request handler is replay-safe: a
repeated `request_id` returns the cached response and cannot debit twice. No
consumer correctness path depends on `fluid.flow`; an optional flow event is
telemetry only.

PipeNetwork fills pipe buffers through accepted drain responses and drains pipe
buffers for a consumer before requesting a source drain. Mixed resource IDs are
never combined. A source with a different `fluid_id` is excluded and receives a
blocked/short-fill result.

### 4. Craft orchestration

RecipeManager remains the recipe catalog and condition evaluator. SimulationCore
owns the machine lifecycle and a `PendingCraft` state. It resolves a recipe,
requests/reserves every required resource, and starts the recipe only when the
required accepted amount is returned. Input items are consumed at the same
commit point, not when a recipe is merely discovered. A zero/partial response
leaves the recipe pending, applies timeout/backoff, and never advances progress.

The resource requirement is generic (`resource_kind`, `resource_id`, amount,
tier), so STEAM and future electricity use the same orchestration contract.

### 5. Client state

Gateway forwards a typed server-authoritative `ResourceBufferState` associated
with a machine/port. Client network callbacks enqueue validated state updates;
the render thread applies them to its state store. UI resolves the display label
and units through the registry and never derives identity from a hardcoded
string, position-key reimplementation, or raw PipeNetwork update in `Run()`.

### 6. Topology and direction

The graph remains one connected topology per resource domain. Direction is a
future edge/port policy checked by the solver; it is not represented by separate
networks in this change.

## Failure handling and migration

- Unknown/removed ports return a zero accepted amount and clear pending state.
- Negative, overflowed, or over-capacity amounts are rejected and logged.
- A service restart re-registers ports and reconstructs only the owner snapshots;
  pipe buffers use the existing persistence path.
- During migration, old mirror `fluid.flow` consumers are disabled before the new
  response path is enabled, preventing double debit.
- Every protocol change gets generated FlatBuffers in all affected targets and
  cross-service contract tests.

## Open Questions

- Should a future fluid item row use the same numeric value as `fluids.csv.id`,
  or should the two remain explicitly distinct with a mandatory mapping? This
  change keeps them distinct concepts but requires the mapping to be explicit.
- Should source drain be a direct request to SimulationCore or routed through a
  dedicated resource coordinator? Start with the existing MessageRouter and
  split a coordinator only when multiple consumers need atomic reservations.
