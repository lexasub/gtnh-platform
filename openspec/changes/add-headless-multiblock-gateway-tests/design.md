# Design: Headless Gateway multiblock testing

## Context

The headless Go client speaks the production control frame directly. SimulationCore owns multiblock controllers; ChunkStore owns block, meta, and `mb_id`; EntityStateStore owns persisted multiblock blobs. The Gateway exposes lifecycle events as type 23 but no controller query.

## Decisions

- Keep the first milestone on the existing Gateway path: place the exact EBF fixture, wait for ACKs and the created event, query ChunkStore for non-zero `mb_id`, then break and verify destruction.
- Use unique coordinates per test and a cleanup path. Place the controller last because formation is checked on the controller block change.
- Harden shared framing with `io.ReadFull` and correlate ACKs by `request_id`; do not add a second wire protocol.
- Treat persistence as a separate integration milestone. Start EntityStateStore with an isolated LMDB and inspect its FlatBuffers RPC directly; do not invent a GUI-only status query.
- Add CLI event decoding so manual scenarios and automated assertions observe the same type-23 wire event.

## Risks / Trade-offs

- Structural/controller IDs 1001–1006 and hatch IDs are currently runtime/test placeholders; fixtures must use the exact values in PatternLibrary and document this dependency.
- Existing integration startup omits EntityStateStore, so persistence tests must add readiness and isolated storage before claiming coverage.
- Gateway push events can interleave with ACKs; readers must skip unrelated messages while retaining typed events.
