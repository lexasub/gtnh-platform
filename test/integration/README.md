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
