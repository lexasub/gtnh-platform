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

## The cluster cannot outlive the test binary

Every service runs under a per-service supervisor (testutil/supervisor.go).
The test binary re-execs itself as a supervisor, hands it the real service, and
keeps the write end of a guard pipe open. When the test binary dies — `os.Exit`,
a `-test.timeout` panic, SIGINT, SIGKILL, anything — the kernel closes that
pipe, the supervisor reads EOF, and it kills its own process group.

This is a kernel-driven signal, not an in-process one, because nothing inside
the test binary can cover SIGKILL or a timeout panic: `os.Exit` skips defers and
the timeout panic is raised on the runtime's alarm goroutine. A `defer` alone
would have been worse than nothing, because it looks like it works.

Two consequences worth knowing:

- **A missing service is a hard failure, never a skip.** `startServices` exits
  non-zero with a `FATAL` banner if any service cannot start, and refuses to
  begin at all if :4000/:5001/:7777/:7778 are already bound. A skip is reported
  as a pass by `go test` and by most CI summaries, so a skipped cluster used to
  produce runs that looked green while testing nothing.
- **Teardown is idempotent and bounded.** `ServiceManager.Shutdown` is safe to
  call repeatedly and from a signal handler; the second Ctrl-C restores the
  default disposition and re-raises, so an interrupt can never become an
  unkillable hang.

## A readiness check is not proof a service is alive

`ReadyCheck` callbacks inspect external surfaces — a port, a Router log line —
and all of those can look ready while the service underneath is failing. The
supervisor reports the service's pid and its exit status back over a handshake
pipe, and `StartService` refuses to accept readiness until the launch has been
confirmed and the service has not already reported an exit. A zombie is not an
escape hatch: the service is the supervisor's child, so an unreaped one still
answers `kill(pid, 0)`, which is why the exit report — not the process table —
is authoritative.

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

## Asserting a pick-up: wait on the cursor, not on the item's absence

`WaitForInventoryItem` only expresses POSITIVE inventory state. A test that
must prove a click moved a stack therefore has to reach for the *absence* of
the item from the grid — and that negative holds in several cases where the
click did nothing: the server dropped the click, the stack landed somewhere
the snapshot does not cover, or another test sharing the player id moved it.
Gateway assigns one player id to every Ctrl connection, so that last case is
not hypothetical in a single-cluster suite.

A pick-up click moves the whole stack out of the grid and onto the
server-owned cursor (`InventoryClick.h:148-153`), and both halves are
published in one frame (`InventoryActionHandler.cpp:96-99`). So assert the
cursor, which is the positive form of the same fact:

```go
if _, err := c.WaitForCursorItem(playerID, itemID, minCount, 5*time.Second); err != nil {
    t.Fatalf("pick-up did not move item %d onto the cursor: %v", itemID, err)
}
```

`WaitForCursorItem` reads `InventoryUpdate.cursor`; `item_id == 0` with
`count == 0` is an empty cursor. Note that the Go accessor returns a
zero-valued struct rather than nil for an absent cursor, so test the fields,
not the pointer.

## Placement frames are guarded in Go, not only in C++

The claim rule lives in `PlaceBlockHandler::isPlacementShape`: a right-click
is a placement only when `held_item != 0` and it is not a mining tool or a
wrench (`PlaceBlockHandler.cpp:32-36`). Nothing in the Go fixture layer
enforces that, so a fixture that regresses to `BuildSetBlockAction` produces a
frame no handler claims, and the only symptom is a `REJECTED` five seconds
later that reads like a server bug.

`TestPlaceAtCellSendsAClaimableFrame` (block_test.go) asserts the whole frame
contract — action type, held item, face, the `y+1` clicked-cell convention,
and request correlation — with no cluster and no timing. Run it after touching
any placement builder or `placeAtCell`.

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
