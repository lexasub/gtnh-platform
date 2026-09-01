## 1. Registry identity and validation

- [x] 1.1 Define the shared canonical `ItemId` type and registry contract;
  `items.csv` owns every item/block/pipe/cable/fluid ID. Do not introduce
  `FluidId`, `PipeId`, or `CableId` types.
- [x] 1.2 Add loaders for `pipes.csv`, `cables.csv`, and `fluids.csv` to the
  shared registry library. Specialized rows contain only canonical `item_id`
  references plus properties; remove their independent object-ID columns.
- [x] 1.3 Validate every `pipes.csv.item_id`, `cables.csv.item_id`, and
  `drops.csv` source/result against `items.csv`; validate names and duplicates.
- [ ] 1.4 Replace hardcoded Steam IDs in SimulationCore, PipeNetwork, tests,
  and client state code with the canonical packed Steam `ItemId` from the shared
  item registry.
  - [ ] 1.4.1 Add one shared Steam-ID lookup/accessor used by server resource
    producers and consumers.
  - [ ] 1.4.2 Replace Steam literals in `MachineSystem`, boiler/generator
    systems, fluid clients, and pipe tests.
  - [ ] 1.4.3 Replace Steam literals in client state/UI code and ensure labels
    come from the registry rather than string or numeric constants.
  - [ ] 1.4.4 Add a test that the resolved Steam ID equals the `items.csv` row
    and the `fluids.csv` mapping.
- [x] 1.5 Add registry validation tests, including unknown references,
  duplicate IDs, mismatched names, and valid Steam/pipe/cable/drop rows.

## 2. Typed resource ports

- [x] 2.1 Add `PortId`, `ResourceKind` (`FLUID`, `EU`, `HU`, `RU`, `ITEM`),
  `PortRole`, and `ResourcePort` types in a shared protocol/domain library.
  Do not add resource filters; concrete FLUID/ITEM transfer messages carry the
  actual `fluid_id`/`item_id`.
- [x] 2.2 Extend registration/update messages with port identity, resource kind,
  resource ID, owner identity, epoch, role, capacity, rate, and face policy.
- [ ] 2.3 Refactor PipeNetwork to keep independent graphs/state for each
  resource domain; do not reuse one `is_source`/`is_sink` pair for HEAT and FLUID.
  - [ ] 2.3.1 Define the manager-side typed graph/state model for FLUID, EU,
    HU, RU, and ITEM without sharing domain role flags.
  - [ ] 2.3.2 Route typed port registrations into the corresponding domain and
    prevent a port from being consumed by another resource kind.
  - [ ] 2.3.3 Keep topology shared where appropriate while applying resource
    kind, role, face policy, capacity, and rate during solving.
  - [ ] 2.3.4 Add a compatibility boundary for legacy node updates and document
    the removal point after all producers migrate.
- [ ] 2.4 Register the heat boiler as separate HEAT-sink and STEAM-source ports;
  make registration idempotent and order-independent.
  - [ ] 2.4.1 Allocate stable distinct port IDs/slots for the boiler HU sink and
    FLUID Steam source.
  - [ ] 2.4.2 Publish both registrations with owner, epoch, role, capacity, rate,
    position, and face policy.
  - [ ] 2.4.3 Verify boiler registration works in either publication order and
    repeated ticks do not create duplicates.
  - [ ] 2.4.4 Add a converter test proving HU input and Steam output roles remain
    independent.
- [ ] 2.5 Unregister all ports and pending requests when a machine is removed,
  replaced, or its chunk entity is destroyed.
  - [ ] 2.5.1 Handle typed `ResourcePortRemove` for exact `(owner, kind, port,
    epoch)` removal.
  - [ ] 2.5.2 Remove all ports and pending transactions during owner/entity
    destruction or replacement.
  - [ ] 2.5.3 Reject stale epoch updates and clear requests associated with a
    removed port.
  - [ ] 2.5.4 Add removal/re-registration tests, including owner ID zero and
    manager/EnTT ID collisions.
- [ ] 2.6 Add tests for converters, simultaneous ports, entity ID zero, manager
  ID collisions, re-registration, and removal cleanup.
  - [ ] 2.6.1 Test simultaneous HU, FLUID, EU, and ITEM ports on one owner.
  - [ ] 2.6.2 Test converter registration and independent source/sink roles.
  - [ ] 2.6.3 Test zero entity IDs, manager ID collisions, duplicate updates,
    stale epochs, and exact removal.
  - [ ] 2.6.4 Test that removal also clears pending requests and replay state.

## 3. Transactional resource accounting

- [x] 3.1 Add request/response protocol tables with `request_id`, `port_id`,
  channel kind, canonical packed item ID when applicable, requested amount,
  accepted amount, and remaining.
- [ ] 3.2 Implement SimulationCore owner-side drain handlers with capacity and
  availability guards; ensure one debit per request ID.
  - [ ] 3.2.1 Subscribe to typed `ResourceDrainRequest` messages and validate
    owner, port, resource kind, resource ID, epoch, and positive amount.
  - [ ] 3.2.2 Implement FLUID source draining against `SteamOutputComponent`/
    `FluidStorage` without mutating pipe-owned buffers.
  - [ ] 3.2.3 Add owner-side replay storage keyed by request ID and return the
    cached response without a second debit.
  - [ ] 3.2.4 Return zero/short acceptance for removed, mismatched, unavailable,
    over-capacity, negative, or overflowed requests.
- [ ] 3.3 Implement PipeNetwork pipe-buffer accounting; remove source/machine
  mirror writes and make flow events telemetry-only.
  - [ ] 3.3.1 Make typed pipe buffers the only authoritative transport-side
    amount and keep machine buffers authoritative in SimulationCore.
  - [ ] 3.3.2 Replace all remaining source/machine mirror writes in service and
    reactor paths with typed request/response operations.
  - [ ] 3.3.3 Ensure `fluid.flow` carries telemetry only and cannot debit or
    credit owner state on replay.
  - [ ] 3.3.4 Add conservation assertions across pipe, source, and destination
    states for accepted, rejected, and disconnected transfers.
- [ ] 3.4 Implement pipe-first consume, source shortfall requests, exact
  accepted amounts, and blocked/mismatched-fluid responses. PipeNetworkService
  now consumes pipe buffers first and computes the source shortfall with exact
  consumed/remaining amounts, but owner-side source drain requests are still
  missing; the service currently updates its source snapshot directly.
  - [x] 3.4.1 Consume only the matching fluid from the destination pipe buffer
    and return exact accepted/remaining values.
  - [ ] 3.4.2 Emit a typed source-drain request for only the remaining shortfall.
  - [ ] 3.4.3 Apply the accepted source response exactly once and return the
    combined accepted amount to the consumer.
  - [ ] 3.4.4 Return blocked/short-fill responses for mismatched fluid, missing
    ports, insufficient capacity, and disconnected networks.
- [ ] 3.5 Add replay cache, request expiry, timeout/backoff, and reconnect
  re-registration behavior. Pipe-side replay/TTL support now includes
  request-tuple binding and non-caching request ID zero; service-level request
  identity, timeout/backoff, and reconnect re-registration remain pending.
  - [x] 3.5.1 Cache pipe-side nonzero request IDs with TTL and reject a reused ID
    whose node, fluid, or amount tuple differs.
  - [ ] 3.5.2 Carry request IDs through service, SimulationCore, and response
    routing instead of relying on FIFO or position correlation.
  - [ ] 3.5.3 Add bounded timeout, retry backoff, expiry, and cancellation for
    pending drain/consume requests.
  - [ ] 3.5.4 Re-register ports after service restart/reconnect and discard
    responses from old epochs or removed ports.
- [ ] 3.6 Add tests for exact conservation, partial fills, duplicate requests,
  stale responses, mixed fluids, and source/sink capacity limits. Focused
  partial/replay/mismatch coverage exists; the complete matrix remains pending.
  - [ ] 3.6.1 Test exact conservation for pipe-only, source-only, and combined
    shortfall transfers.
  - [ ] 3.6.2 Test partial/zero acceptance without over-debiting either owner.
  - [ ] 3.6.3 Test duplicate IDs, tuple conflicts, request ID zero, expiry, and
    stale epoch responses.
  - [ ] 3.6.4 Test mixed fluids and source/sink capacity boundaries.

## 4. Recipe orchestration

- [ ] 4.1 Add generic resource requirements to the machine/recipe execution
  contract (`kind`, `resource_id`, amount, tier).
  - [ ] 4.1.1 Add a serializable `ResourceRequirement` with kind, canonical
    resource ID, amount, and tier to the recipe model/protocol.
  - [ ] 4.1.2 Parse resource requirements from YAML while preserving existing
    `energy_in`/`eu` compatibility during migration.
  - [ ] 4.1.3 Validate resource kind/ID/tier against the machine and shared
    registry at recipe load/startup.
  - [ ] 4.1.4 Expose all requirements to MachineSystem, EBFSystem, and LCRSystem
    through one execution contract.
- [ ] 4.2 Add `PendingCraft`/reservation state to SimulationCore and request
  resources before consuming recipe input items.
  - [ ] 4.2.1 Add PendingCraft lifecycle state to RecipeProgress, including
    recipe identity, owner/entity, request IDs, required and accepted amounts,
    epoch, retry count, and expiry tick.
  - [ ] 4.2.2 Reserve/request every requirement before mutating input inventory
    or starting progress.
  - [ ] 4.2.3 Persist and restore PendingCraft state with machine/multiblock
    state where the existing persistence boundary permits.
  - [ ] 4.2.4 Cancel pending reservations on machine removal, recipe change,
    disconnect, timeout, or failed validation.
- [ ] 4.3 Start and advance a recipe only after the required accepted amount is
  returned; handle zero/partial response without consuming inputs.
  - [ ] 4.3.1 Correlate each response by request ID and requirement, not FIFO.
  - [ ] 4.3.2 Commit input consumption and initial progress only after all
    required reservations are fully accepted.
  - [ ] 4.3.3 Keep zero/partial responses pending without consuming inputs or
    advancing progress; apply bounded retry/backoff.
  - [ ] 4.3.4 Charge recurring per-tick resource requirements through the same
    accepted-amount path before decrementing active recipe progress.
- [ ] 4.4 Migrate Steam recipes for all Steam machines to explicit resource costs
  and validate `energy_in` against the machine registry.
  - [ ] 4.4.1 Add explicit Steam costs to compressor, extractor, mixer, and all
    Steam macerator recipes that currently omit or duplicate a cost.
  - [ ] 4.4.2 Audit every Steam-capable machine variant and recipe class for
    missing, zero, or mismatched `energy_in` declarations.
  - [ ] 4.4.3 Add explicit/validated resource costs for EBF HEAT and LCR/EU paths
    where their machine registry contract requires an external resource.
  - [ ] 4.4.4 Fail recipe validation for an `energy_in` value incompatible with
    the registered machine class or tier.
- [ ] 4.5 Add recipe tests proving no Steam request means no recipe start and
  that Steam and future electricity share the orchestration path.
  - [ ] 4.5.1 Test no request, zero acceptance, and partial Steam acceptance:
    inputs and progress remain unchanged.
  - [ ] 4.5.2 Test full Steam acceptance commits inputs exactly once and starts
    progress.
  - [ ] 4.5.3 Test recurring Steam charge before active progress advancement.
  - [ ] 4.5.4 Test an EU requirement through the same reservation interface and
    reject mismatched machine energy declarations.

## 5. Server-authoritative client state

- [ ] 5.1 Add a typed machine/port resource-state message routed by Gateway,
  separate from internal PipeNetwork update messages.
  - [ ] 5.1.1 Define `ResourceBufferState` with owner/port identity, kind,
    canonical resource ID, amount, capacity, rate, epoch, and sequence.
  - [ ] 5.1.2 Publish state from SimulationCore and route it through Gateway to
    the correct client connection.
  - [ ] 5.1.3 Keep internal PipeNetwork registration/transaction messages out
    of the client-facing route.
- [ ] 5.2 Add a client state-store queue with render-thread application and
  lifecycle/sequence handling.
  - [ ] 5.2.1 Add a thread-safe queue for validated resource-state updates.
  - [ ] 5.2.2 Apply updates on the render thread only when owner/port epoch and
    sequence are current.
  - [ ] 5.2.3 Discard stale, removed-port, malformed, or out-of-order updates.
  - [ ] 5.2.4 Clear state on entity/chunk removal and reconnect.
- [ ] 5.3 Remove raw PipeNetwork decoding from `GameClient::Run()`, duplicated
  position-key encodings, and hardcoded fluid labels.
  - [ ] 5.3.1 Remove transport-message decoding from the render loop and use the
    client state store as the only UI input.
  - [ ] 5.3.2 Reuse a shared position/port identity representation instead of
    reimplementing service position-key packing in the client.
  - [ ] 5.3.3 Replace hardcoded Steam/fluid labels and units with registry lookup
    and typed channel metadata.
- [ ] 5.4 Resolve resource names and units through the canonical registry and
  add client/server cross-service contract tests.
  - [ ] 5.4.1 Resolve canonical item/fluid names and display units through the
    shared registry contract.
  - [ ] 5.4.2 Add a FlatBuffers client/server fixture covering state publication,
    Gateway routing, queue application, sequence, and removal.
  - [ ] 5.4.3 Test unknown IDs and mismatched epochs fail closed without corrupting
    displayed state.

## 6. Cleanup and verification

- [x] 6.1 Remove or migrate `FluidFlowHandler` mirror-debit behavior; retain
  optional flow telemetry only. Source-drain flow events are telemetry-only;
  destination delivery remains explicitly handled by the destination path.
- [x] 6.2 Keep directional routing as a future edge/port policy; do not split
  graphs by direction in this change.
- [x] 6.3 Remove unrelated wrench/UI/CLI changes from the implementation diff
  or track them under separate changes.
- [x] 6.4 Run generated FlatBuffers updates, incremental `ninja -j5`, full ctest,
  registry validation, and `openspec validate refactor-fluid-port-accounting --strict`.
  Incremental build, focused tests, registry validation, and strict OpenSpec
  validation pass. Full ctest passes all 18 enabled tests; `toctou_test` remains
  intentionally disabled.
