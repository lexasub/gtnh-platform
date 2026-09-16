# Headless Gateway client

The repository has two headless clients for exercising the real Gateway TCP path without starting the GUI game client:

- **Automated client:** `test/integration/testutil/client.go` defines `GatewayClient`, which connects to the control port, sends the wire frame `[4-byte BE length][1-byte GatewayMsg][FlatBuffer]`, and waits for typed responses. The integration suite in `test/integration/` uses it for block CAS, chunk persistence, machine slots, crafting, reconnects, and the bulk-port smoke test.
- **Diagnostic CLI:** `tools/gateway_cli/` is a standalone Go program for manual reproduction. It supports `place`, `break`, `open`, `slot`, `pipe`, and multi-step `script` scenarios. It prints block acknowledgements and selected machine/pipe responses, so a bug can be reproduced through Gateway without a GUI session.

The client-facing wire IDs come from `src/common/GatewayMsg.h`; the `GatewayPayload` union in `src/protocol/gateway.fbs` is stale and must not be used as the numeric source of truth. Gateway control frames are `[4 bytes big-endian payload size][1 byte message type][FlatBuffer]`. The headless clients are protocol clients, not alternate simulation implementations: block placement still travels through Gateway → MessageRouter → SimulationCore → ChunkStore.

## Current multiblock coverage

The C++ SimulationCore tests already form an EBF in-process (`src/services/simulation_core/test/test_ecs_systems.cpp`) and verify hatch layout, slot allocation, and serialization. The headless Gateway path does not yet have an equivalent end-to-end test. In particular:

- `testutil` can build place/break `SetBlockAction` messages and assert `BlockAck`, but does not decode `kMultiblockEvent` (wire type 23).
- `gateway_cli` can place and break blocks, but its reader does not print multiblock-created/destroyed events.
- `test/integration/main_test.go` starts Router, Gateway, ChunkStore, SimulationCore, and MetaDB, but not EntityStateStore. Therefore it cannot currently prove multiblock persistence through the production save/load path.
- `SimulationEngine` exposes controller state only in-process. Persistence assertions need either direct EntityStateStore RPC inspection or a deliberately added diagnostic seam; they should not infer controller state from a GUI-only effect.

## Extension plan: E2E multiblock scenarios

Implement the extension in small layers so a failed test identifies the broken service boundary:

1. **Make the client reliable and correlation-aware.** Use `io.ReadFull` for frame reads, preserve `request_id` in all builders, add `WaitBlockAck(requestID, status)`, and add typed decoders for `MultiblockCreatedEvent` and `MultiblockDestroyedEvent`. Keep the existing skip-unexpected-message behavior because Gateway push events may be interleaved with acknowledgements.
2. **Add a deterministic EBF fixture.** Reserve a unique, isolated coordinate region and place the 3×4×3 EBF from `src/services/simulation_core/ECS/PatternLibrary.cpp`: casing `1001`, coils `1002`, and controller `1003`. Resolve the current placeholder hatch IDs from `PatternLibrary.h` rather than inventing a second ID mapping. Place the controller last, as the pattern check runs on the controller change. Assert every placement ACK before moving to the next layer.
3. **Assert formation through the actual Gateway path.** Wait for type-23 `MultiblockCreatedEvent` and verify controller ID, anchor, and pattern type. Query ChunkStore for the structure positions and assert the committed `mb_id` is non-zero, proving that SimulationCore wrote multiblock metadata rather than only publishing an event.
4. **Assert teardown.** Break a non-anchor block and verify the controller remains active; then break the anchor and wait for `MultiblockDestroyedEvent`. Query ChunkStore again to verify the relevant block state and `mb_id` cleanup. Clean the reserved region in a test cleanup path so retries do not inherit stale CAS state.
5. **Add persistence as a separate integration test.** Start `entitystated` with an isolated LMDB directory in `TestMain`, form a structure, put state into a hatch/controller when the existing slot protocol supports it, trigger the production save path, and inspect EntityStateStore using its FlatBuffers RPC (`entity_type=4`, multiblock state). Only after this works should the test cover restore/reformation; the current implementation saves on controller destruction, so an ordinary active tick is not a persistence boundary.
6. **Expose the same observability in the CLI.** Add a `watch-multiblock` mode (or script command) that decodes type 23 and prints created/destroyed events, plus explicit `--request-id`/expected-status options. This keeps manual reproduction and automated assertions on the same wire behavior.

The first milestone should be formation + metadata + teardown. Persistence is a second milestone because it requires an additional service and direct RPC assertions; adding a client-only “multiblock status” query would hide rather than test the existing ownership boundary (SimulationCore owns controllers, ChunkStore owns only block/meta/`mb_id`).

## Thermal power-chain coverage

The next headless milestone is tracked separately in `openspec/changes/add-headless-thermal-power-chain/`. It exercises multiple heat generators, steam heat boilers, heat/fluid pipe segments, steam turbines, battery buffers, and an LV electric machine whose recipe consumes 32 EU/t. The first probe is `test/integration/energy_chain_test.go`; it requires `pipenetworkd` and uses correlated block acknowledgements. Full green state-transition assertions remain gated on the production turbine, rechargeable-battery, and flow-observability contracts. Gateway-visible state is limited to block-entity/resource-buffer snapshots and the read-only fluid `PipeContentsResp`; exact internal energy topology requires Router telemetry or focused C++ tests.

## Running the existing clients

Start the normal services first (at minimum Router, ChunkStore, Gateway, and SimulationCore; add MetaDB/EntityStateStore for persistence scenarios), then run:

```bash
# Automated integration suite
cd test/integration
go test ./...

# Manual diagnostic client
cd ../../tools/gateway_cli
go run . --addr 127.0.0.1:7777 place 400 70 401 0xE400
go run . --addr 127.0.0.1:7777 script /tmp/scenario.txt
```

Use isolated coordinates and explicit expected block IDs. The CLI and tests mutate the live world; they are not read-only probes.
