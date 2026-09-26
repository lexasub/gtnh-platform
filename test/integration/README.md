# Integration test harness

`go test` here starts real service binaries. The process shape decides which
tests can be repeated, so read this before adding a tracer.

## One cluster per test process

`TestMain` (main_test.go) starts one set of services — router, chunkd,
entitystated, gatewayd, simcored, metadbd, pipenetworkd — and `m.Run()` executes
every test against that same cluster. Service startup is not idempotent across
repetitions, and server state written during the first run is visible to the
second.

Gateway assigns **player 1** to every Ctrl connection
(`src/apps/gateway/gateway.cpp`, `client_player_id_ = 1`), so two tests in one
process share one player identity.

## Single-run contract

A test that proves a one-shot server effect must claim its run before doing any
work:

```go
if err := testutil.ClaimSingleRun(t.Name()); err != nil {
    t.Fatalf("repeat run: %v", err)
}
```

`ClaimSingleRun` (testutil/single_run.go) returns a `*SingleRunError` on the
second and later invocation in the same process. Without it, the repeat fails
late and misleadingly — for example
`TestQuestBookOpen_InventoryCompletion` waits 5 s and reports
`read tcp ...: i/o timeout`, which reads like a transport bug rather than
"quest 1 is already completed from the previous run".

Use `t.Name()` as the key so renaming a test cannot silently mint a second
claim slot.

The contract is opt-in. A genuinely repeatable test simply does not call it.

## Placement frames carry a held item

`BuildSetBlockAction` builds an **empty hand**: `held_item = 0`. That is not a
placement. `PlaceBlockHandler::canHandle` requires `held_item != 0`
(`src/game/actions/handlers/PlaceBlockHandler.cpp:14-18`), so an empty-hand
RIGHT_MOUSE_CLICK is claimed by no handler and the facade answers `REJECTED`
with "nothing placeable in hand" (`SetBlockCASHandler.cpp:44-51`).

The production client never sends it — `NetClient::SendBlockAction` writes the
equipped item into both `new_block_id` and `held_item`
(`NetClient.cpp:689-706`). Use `BuildPlaceBlockAction` (or
`BuildPlaceBlockActionWithOptions` when you need a request id):

```go
fb := testutil.BuildPlaceBlockAction(playerID, x, y, z, expectedID, blockID)
```

`BuildSetBlockAction` stays for raw-protocol and negative-path tests that
deliberately exercise an empty hand.

## Placement geometry is derived server-side

A placement does **not** CAS against the client's `expected_block_id`.
`ActionContext` resolves the effective cell with `faceAdjacentBlock()` and then
forces `eff_expected = 0` — "place against air"
(`ActionContext.cpp:44-49`). Two consequences for every placement fixture:

- The frame must carry the clicked cell, and `Face: 0` (DOWN, the top face of
  the block below), so the server's `--y` lands on the cell you meant. Sending
  `y` with `Face: 0` places the block at `y-1`.
- The target cell must actually be air. A fixed `y` makes the fixture depend on
  the terrain height the world generator happened to produce; use
  `airCellAbove` to resolve a real empty cell from ChunkStore first.

Gateway only requests the chunk at the player's saved position
(`gateway.cpp:116-131`), so a placement far from spawn must call
`RequestChunk` + `WaitForChunkGeneration` or the CAS loses the world-gen race
and comes back `CONFLICT`.

## `ACCEPTED` is optimistic

`runBlockCas` publishes `ACCEPTED` before the ChunkStore CAS runs
(`CasRunner.cpp:15-17`); the authoritative result arrives later as `CONFLICT`
(`CasRunner.cpp:44-46`). To assert a placement actually happened, check
ChunkStore — `PlaceBlockAndWait` and `WaitForBlock` do that.

## Item ids are packed

Block and item ids are packed with `ItemId::pack`
(`src/engine/registry/ItemId.h:85-128`). Legacy flat literals like `7`, `13`,
`36` and `46` name nothing in the current registry. Derive them from
`src/content/data/registry/items.csv` and `machines.yaml`:

| id | packed form | decimal |
|----|-------------|---------|
| cobblestone | `0:0:2` | 2 |
| stone | `0:0:1` | 1 |
| oak_planks | `0:10:00:0` | 16384 |
| crafting_table | `0:10:11:1` | 22529 |
| stick | `0:11110:0` | 30720 |
| iron_ingot | `0:110:1` | 24577 |
| iron_ore | `10:0` | 32768 |
| coal | `0:11110:2` | 30722 |
| heat_furnace | `1110:000:0` | 0xE000 |
| heat_macerator | `1110:000:1` | 0xE001 |
| heat_generator | `1110:000:2` | 0xE002 |
| steam_compressor | `1110:001:1` | 0xE201 |
| creative_generator | `1110:100:0` | 0xE800 |
| item_pipe | `1111:10:0` | 0xF800 |
| tin_cable | `1111:01:0` | 0xF400 |

## Chunk data is a bulk-port message

`world.chunk.loaded.compressed` is forwarded to the **bulk** connection only,
never ctrl (`gateway.cpp:387-393`), and dropped with "no bulk client" when no
bulk socket is open. Read `MsgCompressedChunk` from port 7778, after opening
the ctrl connection that triggers the `CHUNK_REQUEST`.

## Correlating server pushes

Queued pushes such as `player.inventory.update` are forwarded without
correlation, so never consume "the first frame of this type". Wait for the
snapshot that satisfies the predicate:

```go
data, err := c.WaitForInventoryItem(playerID, itemID, minCount, 5*time.Second)
```

`WaitForInventoryItem` skips empty payloads, other players, other items and
lower totals, and sums counts across slots because the server splits stacks.

## Running

```bash
go test ./testutil            # unit tests only, no cluster, seconds
go test -run '^TestX$' -count=1 -v   # a focused tracer, one run per process
```

Note that `go test ./...` also runs the root package, which starts the full
cluster. It takes minutes, and on this tree several tests in it are known to
fail (they predate the Router-barrier work - see the beads issue for evidence).
Use the focused form while developing.

`-count>1` is a valid stability gate for `./testutil` (no cluster), and is
rejected by contract for single-run tracers.
