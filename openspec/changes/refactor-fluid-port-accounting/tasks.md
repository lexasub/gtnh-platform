## 1. Registry identity and validation

- [ ] 1.1 Define the shared canonical `ItemId` type and registry contract;
  `items.csv` owns every item/block/pipe/cable/fluid ID. Do not introduce
  `FluidId`, `PipeId`, or `CableId` types.
- [ ] 1.2 Add loaders for `pipes.csv`, `cables.csv`, and `fluids.csv` to the
  shared registry library. Specialized rows contain only canonical `item_id`
  references plus properties; remove their independent object-ID columns.
- [ ] 1.3 Validate every `pipes.csv.item_id`, `cables.csv.item_id`, and
  `drops.csv` source/result against `items.csv`; validate names and duplicates.
- [ ] 1.4 Replace hardcoded Steam IDs in SimulationCore, PipeNetwork, tests,
  and client state code with the canonical packed Steam `ItemId` from the shared
  item registry.
- [ ] 1.5 Add registry validation tests, including unknown references,
  duplicate IDs, mismatched names, and valid Steam/pipe/cable/drop rows.

## 2. Typed resource ports

- [ ] 2.1 Add `PortId`, `ResourceKind` (`FLUID`, `EU`, `HU`, `RU`, `ITEM`),
  `PortRole`, and `ResourcePort` types in a shared protocol/domain library.
  Do not add resource filters; concrete FLUID/ITEM transfer messages carry the
  actual `fluid_id`/`item_id`.
- [ ] 2.2 Extend registration/update messages with port identity, resource kind,
  resource ID, owner identity, epoch, role, capacity, rate, and face policy.
- [ ] 2.3 Refactor PipeNetwork to keep independent graphs/state for each
  resource domain; do not reuse one `is_source`/`is_sink` pair for HEAT and FLUID.
- [ ] 2.4 Register the heat boiler as separate HEAT-sink and STEAM-source ports;
  make registration idempotent and order-independent.
- [ ] 2.5 Unregister all ports and pending requests when a machine is removed,
  replaced, or its chunk entity is destroyed.
- [ ] 2.6 Add tests for converters, simultaneous ports, entity ID zero, manager
  ID collisions, re-registration, and removal cleanup.

## 3. Transactional resource accounting

- [ ] 3.1 Add request/response protocol tables with `request_id`, `port_id`,
  channel kind, canonical packed item ID when applicable, requested amount,
  accepted amount, and remaining.
- [ ] 3.2 Implement SimulationCore owner-side drain handlers with capacity and
  availability guards; ensure one debit per request ID.
- [ ] 3.3 Implement PipeNetwork pipe-buffer accounting; remove source/machine
  mirror writes and make flow events telemetry-only.
- [ ] 3.4 Implement pipe-first consume, source shortfall requests, exact
  accepted amounts, and blocked/mismatched-fluid responses.
- [ ] 3.5 Add replay cache, request expiry, timeout/backoff, and reconnect
  re-registration behavior.
- [ ] 3.6 Add tests for exact conservation, partial fills, duplicate requests,
  stale responses, mixed fluids, and source/sink capacity limits.

## 4. Recipe orchestration

- [ ] 4.1 Add generic resource requirements to the machine/recipe execution
  contract (`kind`, `resource_id`, amount, tier).
- [ ] 4.2 Add `PendingCraft`/reservation state to SimulationCore and request
  resources before consuming recipe input items.
- [ ] 4.3 Start and advance a recipe only after the required accepted amount is
  returned; handle zero/partial response without consuming inputs.
- [ ] 4.4 Migrate Steam recipes for all Steam machines to explicit resource costs
  and validate `energy_in` against the machine registry.
- [ ] 4.5 Add recipe tests proving no Steam request means no recipe start and
  that Steam and future electricity share the orchestration path.

## 5. Server-authoritative client state

- [ ] 5.1 Add a typed machine/port resource-state message routed by Gateway,
  separate from internal PipeNetwork update messages.
- [ ] 5.2 Add a client state-store queue with render-thread application and
  lifecycle/sequence handling.
- [ ] 5.3 Remove raw PipeNetwork decoding from `GameClient::Run()`, duplicated
  position-key encodings, and hardcoded fluid labels.
- [ ] 5.4 Resolve resource names and units through the canonical registry and
  add client/server cross-service contract tests.

## 6. Cleanup and verification

- [ ] 6.1 Remove or migrate `FluidFlowHandler` mirror-debit behavior; retain
  optional flow telemetry only.
- [ ] 6.2 Keep directional routing as a future edge/port policy; do not split
  graphs by direction in this change.
- [ ] 6.3 Remove unrelated wrench/UI/CLI changes from the implementation diff
  or track them under separate changes.
- [ ] 6.4 Run generated FlatBuffers updates, incremental `ninja -j5`, full ctest,
  registry validation, and `openspec validate refactor-fluid-port-accounting --strict`.
