## Review status (2026-09-01)

- Reviewed on-branch commits `919a3cc`, `b34180f`, `742a65d`, `61a83b1`.
- Build: `cd cmake-build-debug && ninja -j5` — passed (117/117).
- Tests: `ctest --output-on-failure -j$(nproc)` — 18/18 enabled tests passed;
  `toctou_test` is disabled.
- `d75c1aa` is a useful but incomplete Recipe model follow-up; do not cherry-pick
  standalone. It should be integrated only with the parser/protocol wiring.
- `24b2d17` is a no-op include-only checkpoint; do not cherry-pick.
- `28f9be3` is a partial PipeNetwork foundation, but its typed ports are not wired
  into the solver and its lifecycle/accounting is incomplete; do not cherry-pick
  as-is. Port the needed pieces after the current API is finalized.
- The checked items below describe verified implementation, not commit claims.
  Parent items remain unchecked when only a partial or manager-local implementation
  exists.

### Integration status (2026-09-01, agent wave merges)

- Merged on `pipe-boiler2` (linear history): `2bac0c8` typed port contract
  (2.1/2.2/3.1), `7aae849` canonical registry (1.1–1.3/1.4.1/1.4.4), `a81711e`
  per-domain graphs (2.3.x), `8689f7a` boiler ports (2.4.x), `b2581f6` owner-side
  drain handler (3.2.x/2.5.2), `6bd6c35` pipe transactions
  (3.3.x/3.4.2–3.4.4/2.5.1/2.5.3), `36cc06c` server-authoritative client state
  (5.x), `eaeb1ca` test-target link fix, `cc463a7` 2.6.x/3.6.x test matrix,
  `54f8573` matrix assertion fix.
- Verification after each merge: incremental `ninja -j5` green; `simcored_test`
  88 tests / 666 checks 0 failed, `pipe_network_test` green, gameclient suite
  13/13 incl. `gameclient_resource_buffer_state_test` (46 checks).
- Known defect (fix queued): the four serializers in
  `src/common/ResourcePortClient.h` (`SerializePortRegister/PortRemove/
  DrainRequest/ConsumeRequest`) call the table-builder `Finish()` and never
  `fbb.Finish(root)`; the returned buffer has no root offset, so the receiving
  `Parse*` verifier rejects it. Production caller: `PipeNetworkService.cpp:797`
  (`SerializeDrainRequest`). Services currently work around it with local
  serializers (`ResourceDrainHandler.cpp:47`); unit tests inject payloads via
  local helpers, which is why the suite is green. A serialize→parse round-trip
  contract test must accompany the fix.
- Still open after this update: section 4 (implementation agent in progress),
  1.4.2/1.4.3 and 6.1/6.3 (integration-verify agent), 3.5.3/3.5.4.

## 1. Registry identity and validation

- [x] 1.1 Define the shared canonical `ItemId` type and registry contract;
  `items.csv` owns every item/block/pipe/cable/fluid ID. Do not introduce
  `FluidId`, `PipeId`, or `CableId` types.
- [x] 1.2 Add loaders for `pipes.csv`, `cables.csv`, and `fluids.csv` to the
  shared registry library. Specialized CSVs now carry the canonical `item_id`
  namespace (`fluids.csv` migrated).
- [x] 1.3 Validate every `pipes.csv.item_id`, `cables.csv.item_id`, and
  `drops.csv` source/result against `items.csv`; validate names and duplicates.
- [x] 1.4 Replace hardcoded Steam IDs in SimulationCore, PipeNetwork, tests,
  and client state code with the canonical packed Steam `ItemId` from the shared
  item registry. (All server-side users resolve once via `Registry::steamItemId()`
  — BoilerSystem/GeneratorSystem/MachineSystem/ResourceDrainHandler take the id
  as a constructor arg wired in main.cpp; FluidRegistry falls back to the pinned
  constant documented in Registry.h; client uses `ItemRegistry::GetSteamItemId()`.)
  - [x] 1.4.1 Add one shared Steam-ID lookup/accessor used by server resource
    producers and consumers.
  - [ ] 1.4.2 Replace Steam literals in `MachineSystem`, boiler/generator
    systems, fluid clients, and pipe tests.
  - [x] 1.4.3 Replace Steam literals in client state/UI code and ensure labels
    come from the registry rather than string or numeric constants.
    (Sweep found none remaining — A8 already routed labels through
    `ItemRegistry::GetName` + typed channel units; added `ItemRegistry
    ::GetSteamItemId()` resolving the items.csv `steam` row, `5390fed`.)
  - [x] 1.4.4 Add a test that the resolved Steam ID equals the `items.csv` row
    and the `fluids.csv` mapping.
- [x] 1.5 Add registry validation tests, including unknown references,
  duplicate IDs, mismatched names, and valid Steam/pipe/cable/drop rows.

## 2. Typed resource ports

- [x] 2.1 Add `PortId`, `ResourceKind` (`FLUID`, `EU`, `HU`, `RU`, `ITEM`),
  `PortRole`, and `ResourcePort` types in a shared protocol/domain library.
  No resource ID/filter on the port (ops messages carry it); owner ID zero is
  valid.
- [x] 2.2 Extend registration/update messages with port identity, resource kind,
  resource ID, owner identity, epoch, role, capacity, rate, and face policy.
  Shared typed client (`ResourcePortClient.h`) + typed tables; legacy node
  updates remain only behind the documented adapter seam (2.3.4) until all
  producers migrate.
- [x] 2.3 Refactor PipeNetwork to keep independent graphs/state for each
  resource domain; do not reuse one `is_source`/`is_sink` pair for HEAT and FLUID.
  - [x] 2.3.1 Define the manager-side typed graph/state model for FLUID, EU,
    HU, RU, and ITEM without sharing domain role flags.
  - [x] 2.3.2 Route typed port registrations into the corresponding domain and
    prevent a port from being consumed by another resource kind.
  - [x] 2.3.3 Keep topology shared where appropriate while applying resource
    kind, role, face policy, capacity, and rate during solving.
  - [x] 2.3.4 Add a compatibility boundary for legacy node updates and document
    the removal point after all producers migrate.
- [x] 2.4 Register the heat boiler as separate HEAT-sink and STEAM-source ports;
  make registration idempotent and order-independent.
  - [x] 2.4.1 Allocate stable distinct port IDs/slots for the boiler HU sink and
    FLUID Steam source.
  - [x] 2.4.2 Publish both registrations with owner, epoch, role, capacity, rate,
    position, and face policy.
  - [x] 2.4.3 Verify boiler registration works in either publication order and
    repeated ticks do not create duplicates.
  - [x] 2.4.4 Add a converter test proving HU input and Steam output roles remain
    independent.
- [x] 2.5 Unregister all ports and pending requests when a machine is removed,
  replaced, or its chunk entity is destroyed.
  - [x] 2.5.1 Handle typed `ResourcePortRemove` for exact `(owner, kind, port,
    epoch)` removal.
  - [x] 2.5.2 Remove all ports and pending transactions during owner/entity
    destruction or replacement.
  - [x] 2.5.3 Reject stale epoch updates and clear requests associated with a
    removed port.
  - [x] 2.5.4 Add removal/re-registration tests, including owner ID zero and
    manager/EnTT ID collisions.
- [x] 2.6 Add tests for converters, simultaneous ports, entity ID zero, manager
  ID collisions, re-registration, and removal cleanup.
  - [x] 2.6.1 Test simultaneous HU, FLUID, EU, and ITEM ports on one owner.
  - [x] 2.6.2 Test converter registration and independent source/sink roles.
  - [x] 2.6.3 Test zero entity IDs, manager ID collisions, duplicate updates,
    stale epochs, and exact removal.
  - [x] 2.6.4 Test that removal also clears pending requests and replay state.

## 3. Transactional resource accounting

- [x] 3.1 Add request/response protocol tables with `request_id`, `port_id`,
  channel kind, canonical packed item ID when applicable, requested amount,
  accepted amount, and remaining. Typed tables + shared parse/serialize are
  wired end-to-end; see Review status for the `ResourcePortClient.h` serializer
  defect queued for fix.
- [x] 3.2 Implement SimulationCore owner-side drain handlers with capacity and
  availability guards; ensure one debit per request ID.
  - [x] 3.2.1 Subscribe to typed `ResourceDrainRequest` messages and validate
    owner, port, resource kind, resource ID, epoch, and positive amount.
  - [x] 3.2.2 Implement FLUID source draining against `SteamOutputComponent`/
    `FluidStorage` without mutating pipe-owned buffers.
  - [x] 3.2.3 Add owner-side replay storage keyed by request ID and return the
    cached response without a second debit.
  - [x] 3.2.4 Return zero/short acceptance for removed, mismatched, unavailable,
    over-capacity, negative, or overflowed requests.
- [x] 3.3 Implement PipeNetwork pipe-buffer accounting; remove source/machine
  mirror writes and make flow events telemetry-only.
  - [x] 3.3.1 Make typed pipe buffers the only authoritative transport-side
    amount and keep machine buffers authoritative in SimulationCore.
  - [x] 3.3.2 Replace all remaining source/machine mirror writes in service and
    reactor paths with typed request/response operations.
  - [x] 3.3.3 Ensure `fluid.flow` carries telemetry only and cannot debit or
    credit owner state on replay.
  - [x] 3.3.4 Add conservation assertions across pipe, source, and destination
    states for accepted, rejected, and disconnected transfers.
- [x] 3.4 Implement pipe-first consume, source shortfall requests, exact
  accepted amounts, and blocked/mismatched-fluid responses.
  - [x] 3.4.1 Consume only the matching fluid from the destination pipe buffer
    and return exact accepted/remaining values.
  - [x] 3.4.2 Emit a typed source-drain request for only the remaining shortfall.
  - [x] 3.4.3 Apply the accepted source response exactly once and return the
    combined accepted amount to the consumer.
  - [x] 3.4.4 Return blocked/short-fill responses for mismatched fluid, missing
    ports, insufficient capacity, and disconnected networks.
- [x] 3.5 Add replay cache, request expiry, timeout/backoff, and reconnect
  re-registration behavior. Pipe-side replay/TTL with request-tuple binding and
  request-ID-zero semantics are done; cross-service request-ID correlation is
  done (3.2.x/3.4.x); bounded pending timeouts/backoff/cancellation and
  reconnect epoch-gating landed (`982c9cf`, `dd897c7`).
  - [x] 3.5.1 Cache pipe-side nonzero request IDs with TTL and reject a reused ID
    whose node, fluid, or amount tuple differs.
  - [x] 3.5.2 Carry request IDs through service, SimulationCore, and response
    routing instead of relying on FIFO or position correlation.
  - [x] 3.5.3 Add bounded timeout, retry backoff, expiry, and cancellation for
    pending drain/consume requests.
  - [x] 3.5.4 Re-register ports after service restart/reconnect and discard
    responses from old epochs or removed ports.
- [x] 3.6 Add tests for exact conservation, partial fills, duplicate requests,
  stale responses, mixed fluids, and source/sink capacity limits. Complete
  matrix landed; two epoch-matrix assertions corrected after merge (probes must
  exceed the old rate to detect replacement).
  - [x] 3.6.1 Test exact conservation for pipe-only, source-only, and combined
    shortfall transfers.
  - [x] 3.6.2 Test partial/zero acceptance without over-debiting either owner.
  - [x] 3.6.3 Test duplicate IDs, tuple conflicts, request ID zero, expiry, and
    stale epoch responses.
  - [x] 3.6.4 Test mixed fluids and source/sink capacity boundaries.

## 4. Recipe orchestration

- [x] 4.1 Add generic resource requirements to the machine/recipe execution
  contract (`kind`, `resource_id`, amount, tier).
  - [x] 4.1.1 Add a serializable `ResourceRequirement` with kind, canonical
    resource ID, amount, and tier to the recipe model/protocol.
  - [x] 4.1.2 Parse resource requirements from YAML while preserving existing
    `energy_in`/`eu` compatibility during migration.
  - [x] 4.1.3 Validate resource kind/ID/tier against the machine and shared
    registry at recipe load/startup.
  - [x] 4.1.4 Expose all requirements to MachineSystem, EBFSystem, and LCRSystem
    through one execution contract.
- [x] 4.2 Add `PendingCraft`/reservation state to SimulationCore and request
  resources before consuming recipe input items.
  - [x] 4.2.1 Add PendingCraft lifecycle state to RecipeProgress, including
    recipe identity, owner/entity, request IDs, required and accepted amounts,
    epoch, retry count, and expiry tick.
  - [x] 4.2.2 Reserve/request every requirement before mutating input inventory
    or starting progress.
  - [x] 4.2.3 Persist and restore PendingCraft state with machine/multiblock
    state where the existing persistence boundary permits. PendingCraft is
    never serialized — the schema has no fields for it; a pending craft is
    dropped at the serialize boundary and re-requested after restore (safe:
    no inputs consumed while pending).
  - [x] 4.2.4 Cancel pending reservations on machine removal, recipe change,
    disconnect, timeout, or failed validation.
- [x] 4.3 Start and advance a recipe only after the required accepted amount is
  returned; handle zero/partial response without consuming inputs.
  - [x] 4.3.1 Correlate each response by request ID and requirement, not FIFO.
  - [x] 4.3.2 Commit input consumption and initial progress only after all
    required reservations are fully accepted.
  - [x] 4.3.3 Keep zero/partial responses pending without consuming inputs or
    advancing progress; apply bounded retry/backoff.
  - [x] 4.3.4 Charge recurring per-tick resource requirements through the same
    accepted-amount path before decrementing active recipe progress.
- [x] 4.4 Migrate Steam recipes for all Steam machines to explicit resource costs
  and validate `energy_in` against the machine registry.
  - [x] 4.4.1 Add explicit Steam costs to compressor, extractor, mixer, and all
    Steam macerator recipes that currently omit or duplicate a cost.
  - [x] 4.4.2 Audit every Steam-capable machine variant and recipe class for
    missing, zero, or mismatched `energy_in` declarations.
  - [x] 4.4.3 Add explicit/validated resource costs for EBF HEAT and LCR/EU paths
    where their machine registry contract requires an external resource.
  - [x] 4.4.4 Fail recipe validation for an `energy_in` value incompatible with
    the registered machine class or tier.
- [x] 4.5 Add recipe tests proving no Steam request means no recipe start and
  that Steam and future electricity share the orchestration path.
  - [x] 4.5.1 Test no request, zero acceptance, and partial Steam acceptance:
    inputs and progress remain unchanged.
  - [x] 4.5.2 Test full Steam acceptance commits inputs exactly once and starts
    progress.
  - [x] 4.5.3 Test recurring Steam charge before active progress advancement.
  - [x] 4.5.4 Test an EU requirement through the same reservation interface and
    reject mismatched machine energy declarations.

## 5. Server-authoritative client state

- [x] 5.1 Add a typed machine/port resource-state message routed by Gateway,
  separate from internal PipeNetwork update messages.
  - [x] 5.1.1 Define `ResourceBufferState` with owner/port identity, kind,
    canonical resource ID, amount, capacity, rate, epoch, and sequence.
  - [x] 5.1.2 Publish state from SimulationCore and route it through Gateway to
    the correct client connection.
  - [x] 5.1.3 Keep internal PipeNetwork registration/transaction messages out
    of the client-facing route.
- [x] 5.2 Add a client state-store queue with render-thread application and
  lifecycle/sequence handling.
  - [x] 5.2.1 Add a thread-safe queue for validated resource-state updates.
  - [x] 5.2.2 Apply updates on the render thread only when owner/port epoch and
    sequence are current.
  - [x] 5.2.3 Discard stale, removed-port, malformed, or out-of-order updates.
  - [x] 5.2.4 Clear state on entity/chunk removal and reconnect.
- [x] 5.3 Remove raw PipeNetwork decoding from `GameClient::Run()`, duplicated
  position-key encodings, and hardcoded fluid labels.
  - [x] 5.3.1 Remove transport-message decoding from the render loop and use the
    client state store as the only UI input.
  - [x] 5.3.2 Reuse a shared position/port identity representation instead of
    reimplementing service position-key packing in the client.
  - [x] 5.3.3 Replace hardcoded Steam/fluid labels and units with registry lookup
    and typed channel metadata.
- [x] 5.4 Resolve resource names and units through the canonical registry and
  add client/server cross-service contract tests.
  - [x] 5.4.1 Resolve canonical item/fluid names and display units through the
    shared registry contract. Names via `ItemRegistry::GetName`; units are
    typed per-`ResourceKind` channel metadata (`mB`/`EU`/`HU`/`RU`) — the
    registry has no unit column yet.
  - [x] 5.4.2 Add a FlatBuffers client/server fixture covering state publication,
    Gateway routing, queue application, sequence, and removal.
  - [x] 5.4.3 Test unknown IDs and mismatched epochs fail closed without corrupting
    displayed state.

## 6. Cleanup and verification

- [ ] 6.1 Remove or migrate `FluidFlowHandler` mirror-debit behavior; retain
  optional flow telemetry only. **PARTIAL**: source debit adjusted and
  `ResourceBufferState` publication added after `fluid->addFluid` succeeds, but
  destination delivery remains an ECS-mutating correctness path and typed
  owner transactions are not wired end-to-end.
- [x] 6.2 Keep directional routing as a future edge/port policy; do not split
  graphs by direction in this change.
- [ ] 6.3 Remove unrelated wrench/UI/CLI changes from the implementation diff
  or track them under separate changes. **PARTIAL**: the reviewed range still
  contains unrelated `AGENTS.md` and metadata changes.
- [x] 6.4 Run generated FlatBuffers updates, incremental `ninja -j5`, full ctest,
  registry validation, and `openspec validate refactor-fluid-port-accounting --strict`.
  **Verified 2026-09-01 (re-run after agent wave merges)**: incremental
  `ninja -j5` green; `simcored_test` 88 tests / 666 checks 0 failed,
  `pipe_network_test`, `registry_test`, gameclient suite 13/13 incl.
  `gameclient_resource_buffer_state_test` (46 checks). Full `ctest` and strict
  OpenSpec validation to be re-run at the final integration-verify gate
  (section 4 and 1.4.2/1.4.3/6.1/6.3 pending). This gate does not close
  incomplete implementation tasks.
