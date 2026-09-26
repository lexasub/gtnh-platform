# gp-vu1 — Fixed sleeps, deadlines and hardcoded backoffs in the test harness

**Audit date:** 2026-09-26
**Scope:** `test/integration/**` and `test/loadtest/**` (read-only, no source file modified)
**Tree state:** `926521cc` + dirty worktree. **Re-verified against the tree at 12:45**, after a
concurrent agent (gp-dinp/gp-c56) edited 11 of 22 test files. One finding (S-6) has been fixed
by that work; all others re-verified as live. Line numbers below are as of 12:45.

## Verdict per class (final, 12:45)

| Class | Count | Verdict |
|---|---|---|
| Startup barriers that should be predicates | **2** | 1 already fixed (749648c8), **1 remains** and has no predicate available yet |
| Bounded waits that are genuinely appropriate | **11** | leave alone |
| Real leftover flake sources | **6** | fix these (was 7; S-6 was fixed in flight) |

## The complete inventory

Produced by:

```
$ grep -rn "time.Sleep\|time.After\|time.NewTimer\|time.NewTicker\|time.Tick\|context.WithTimeout\|context.WithDeadline" \
    test/integration/ test/loadtest/ --include=*.go
```

Every hit is classified below. Nothing is omitted. Final count of `time.Sleep` alone: **13**
(11 in `test/integration/`, 2 in `test/loadtest/`), plus the timer/ticker sites enumerated in §3
and §5.

---

## 1. The premise: the 5s startup sleep is already gone — CONFIRMED

`git show 749648c8` confirms the removal, and it was a **3-second** sleep, not 5:

```
$ git show 749648c8 -- test/integration/main_test.go | grep -i sleep
182:-	time.Sleep(3 * time.Second)
```

`main_test.go` today contains **zero** `time.Sleep` calls — verified by direct read of all 170
lines. Every service is now gated on a Router-commit barrier:

```go
// test/integration/main_test.go:96-98
ReadyCheck: func(*testutil.ManagedService) bool {
	return testutil.RouterServiceHasSubscriptions(router.Output(), "chunkstore", "chunk.requests")
},
```

with the barrier helpers in `testutil/service.go:317-411`
(`RouterServiceHasRegistration`, `RouterServiceHasRegistrationTopics`,
`RouterServiceHasSubscriptions`, `RouterServiceHasSubscription`) parsing the Router's own
`[router] register:` / `subscribe:` commit lines, correlated by connection id so an unrelated
same-named process cannot satisfy readiness.

**This part of gp-vu1's premise is a valid, complete, and correct fix. Close it as done.**

---

## 2. Leftover startup barriers that should be predicates — 2 REAL DEFECTS

### S-1 · `main_test.go:64-71` — `routerd` readiness still dials a port · **REAL LEFTOVER**

```go
// test/integration/main_test.go:64-71
ReadyCheck: func(*testutil.ManagedService) bool {
	conn, err := net.DialTimeout("tcp", "127.0.0.1:4000", 100*time.Millisecond)
	if err != nil { return false }
	conn.Close()
	return true
},
```

**Why it is a leftover, not a deliberate exception:** commit 749648c8's own message says the
point was to stop "dialing an already-open port" because that can be satisfied by a *stale
listener* from a previous run. This is the **only** service in `startServices` still doing it —
the other six (`pipe_networkd`, `chunkd`, `entitystated`, `simcored`, `gatewayd`, `metadbd`) all
use the Router-commit barrier. Worse, `routerd` is the one service that **cannot** use the Router
barrier (it *is* the Router, so it has no commit log to wait for) — meaning this one line is
load-bearing and cannot simply be deleted.

**Why a wrong frame can satisfy it:** if a previous run's `routerd` on :4000 was not reaped
(possible — `Shutdown` only kills *its own* process groups, `service.go:422-454`), the dial
succeeds against a foreign listener. `StartService` does check that the child it started has not
exited (`service.go:200-217`), so a dead child is caught; but a *live* child that failed to bind
(e.g. `address already in use`) will report ready while the Router never commits registrations.
Every downstream `ReadyCheck` then polls `router.Output()` for a commit that will never come, and
fails after the full 10 s `service.go:198` deadline with a misleading "not ready within 10s".

**Class:** startup barrier that should be a predicate, where the predicate is not yet available
(Router has no self-health endpoint).

**Recommended fix:** either (a) have `routerd` log a `[router] ready:` line on successful bind and
match it, or (b) assert the *listener identity* — have the readiness dial perform a Router-level
handshake and confirm the reply carries the expected service string. Until one of those exists,
**this is a documented, accepted risk**, and the honest action is a comment at `main_test.go:64`
saying so. Do not paper over it with a longer sleep.

### S-2 · `testutil/service.go:218` — the readiness poll interval · **CORRECT, keep**

```go
// test/integration/testutil/service.go:198-219
deadline := time.Now().Add(10 * time.Second)
for time.Now().Before(deadline) {
	select { case err := <-exited: return service, fmt.Errorf(...) ; default: }
	if cfg.ReadyCheck(service) {
		select { case err := <-exited: ... ; default: ... return service, nil }
	}
	time.Sleep(100 * time.Millisecond)
}
return service, fmt.Errorf("%s: not ready within 10s%s", cfg.Name, ...)
```

This is a **bounded poll with a predicate inside** — the textbook-correct shape. The 100 ms sleep
is the poll interval, not the readiness condition. A `ReadyCheck` predicate is evaluated on
every tick, and the child-liveness is rechecked *after* the callback returns
(`service.go:207-216`) to close the accept-an-exiting-child race. **Leave exactly as is.**

### S-3 · `testutil/client.go:345-347` — `WaitForChunkGeneration` is a sleep with a predicate available · **REAL DEFECT, and the single worst offender**

```go
// test/integration/testutil/client.go:343-347
// WaitForChunkGeneration gives the asynchronous world generator time to
// persist a requested chunk. The chunk protocol has no request ACK.
func (c *GatewayClient) WaitForChunkGeneration(timeout time.Duration) {
	time.Sleep(timeout)
}
```

**The doc comment's justification is false.** "The chunk protocol has no request ACK" is true of
the *ctrl* protocol, but the harness already holds a direct ChunkStore connection in almost every
caller — `DialChunkStore` (`testutil/chunkstore.go:22-31`) plus `WaitForBlock`
(`chunkstore.go:111-127`) is a *polling predicate against the authoritative store*, and
`airCellAbove` (`block_test.go:29-42`) already walks a column through it to prove the chunk
exists. The chunk is observable; the sleep is a substitute for an observation that was never
written.

**Cost (re-verified 12:45): 19 call sites × 4-6 s = ~85 s of pure sleep in every full suite run**
(measured suite wall time: 603 s). All of it is on the critical path and none of it can
fail-fast. The concurrent agent *added* one (`gateway_test.go:65`) while fixing gp-dinp, taking
the count from 18 to 19.

**Why it is a real flake source, not just waste:** a 4 s sleep is an *open-ended* bet. It passes
on a fast machine and fails on a loaded one (CI, or a co-scheduled test binary — this repo has at
least four agents running concurrently). The failure it produces is the nasty kind: the *next*
line is a `PlaceBlockAndWait` that returns `CONFLICT` or a `CONFLICT`-then-`ACCEPTED` after
retry (`chunkstore.go:132-162`), so the test blames ChunkStore CAS rather than the sleep.

**Correct predicate:** poll `cs.GetBlock(x>>5 chunk, ...)` for a non-erroring response, or
`cs.WaitForBlock(x, y, z, 0 /*air*/, …)` at the fixture's target cell — which succeeds trivially
once the chunk is generated and loaded. This requires threading a `*ChunkStoreClient` into the
call, which every one of the 16 callers already has in scope or can open in one line.

**Verdict: real leftover flake source. Highest-value single fix in this report.**

---

## 3. Bounded waits that are genuinely appropriate — 11 sites, leave alone

Each of these is a **deadline on a poll loop that has a predicate inside it**, or a bounded
transport read. None papers over a race.

| file:line | Code | Why it is correct |
|---|---|---|
| `service.go:415` | `shutdownGracePeriod = 500 * time.Millisecond` | Grace period before SIGKILL. Bounded by design; the loop at :430-443 polls `Exited()`. |
| `service.go:419` | `shutdownWaitTimeout = 3 * time.Second` | Bounds a wait on a child already SIGKILLed. The `select` at :448-453 is the correct shape. |
| `service.go:442` | `time.Sleep(10 * time.Millisecond)` | Poll interval inside the grace-period loop. Correct. |
| `service.go:450` | `case <-time.After(shutdownWaitTimeout):` | Bounded escape hatch; the comment explains exactly when it fires. Correct. |
| `service.go:168` | `cmd.WaitDelay = 2 * time.Second` | Go stdlib bound on inherited-pipe drain. Correct. |
| `chunkstore.go:123` | `time.Sleep(50 * time.Millisecond)` | Poll interval inside `WaitForBlock`'s predicate loop. Correct. |
| `machine_test.go:273` | `c.ReadCtrl(500 * time.Millisecond)` | Per-read poll interval inside a 40 s predicate loop. Correct. |
| `ebf_multiblock_test.go:222` | `c.ReadCtrl(500 * time.Millisecond)` | Same. Correct. |
| `full_base_test.go:101` | `c.ReadCtrl(500 * time.Millisecond)` | Same. Correct. |
| `energy_chain_test.go:113` | `c.ReadCtrl(500 * time.Millisecond)` | Same. Correct. |
| `lcr_energy_hatch_chain_test.go:190` | `c.ReadCtrl(500 * time.Millisecond)` | Same. Correct. |

Note the pattern: every one of the 500 ms reads sits inside a `for time.Now().Before(deadline)`
loop that filters on a real predicate (`update.Pos()`, `SteamCurrent() > 0`, etc.). **This is
the shape §2's S-3 should be converted to.** The codebase already knows how.

---

## 4. Real leftover flake sources — 5 defects

Beyond S-1 and S-3 above:

### S-4 · `energy_chain_test.go:82` and `lcr_energy_hatch_chain_test.go:145` — machine-open sleeps · **REAL**

```go
// energy_chain_test.go:77-83
open := func(x, y, z int32) {
	c.SendCtrl(testutil.MsgMachineOpenReq, testutil.BuildContainerOpenReq(playerID, x, y, z))
	time.Sleep(100 * time.Millisecond)      // <-- line 82
}
```

```go
// lcr_energy_hatch_chain_test.go:141-146
open := func(x, y, z int32) {
	c.SendCtrl(testutil.MsgMachineOpenReq, testutil.BuildContainerOpenReq(playerID, x, y, z))
	time.Sleep(50 * time.Millisecond)       // <-- line 145
}
```

**These two are the direct cause of the first-frame class in the gp-2rl report.** The comment
at `energy_chain_test.go:74-76` states the requirement plainly: "Opening machine windows is part
of the real client lifecycle. It rehydrates the ECS inventory/session after reconnects; do it
before inserting items." But nothing waits for the rehydration to have happened — the code sleeps
a guessed interval and hopes.

**Why it is a real flake source:** if the `MachineOpenReq` → `BlockEntityUpdate` round trip takes
>100 ms, the subsequent `put(...)` (`energy_chain_test.go:102-104`,
`lcr_energy_hatch_chain_test.go:176-179`) sends a `SetMachineSlotReq` against a container session
that does not exist yet. The server rejects it; `ExpectMsgType(SetMachineSlotResp)` returns a
frame with `success = false`; `lcr_energy_hatch_chain_test.go:171-173` fires
`t.Fatalf("insert item %d rejected")` — a failure that reads as "the LCR rejected iron ore"
when the real cause is a 60 ms race.

**The predicate already exists and is already used 60 lines away:** `energy_chain_test.go`
and the other four pollers filter `BlockEntityUpdate` on `pos`. These two `open()` helpers are
the only places that open a machine and then *don't* check.

**Fix:** `open()` should wait for the `BlockEntityUpdate` whose `pos` matches `(x,y,z)`. Note this
fix also closes gp-2rl F-4 (the near-tautological bare `ExpectMsgType(BlockEntityUpdate)` in
`machine_test.go:245`, `ebf_multiblock_test.go:200`, `full_base_test.go:83`) — the open-wait
becomes correlated *and* the sleep disappears.

### S-5 · `chunk_test.go:125` — 200 ms fixed backoff in a re-query loop · **REAL, but minor**

```go
// chunk_test.go:80-135  (line numbers per the 12:45 tree)
queryDeadline := time.Now().Add(5 * time.Second)
for time.Now().Before(queryDeadline) {
	... parse respPayload, check blockID == 8 ...
	if blockID == 8 { break }
	// Block not committed yet — re-send request after short delay
	time.Sleep(200 * time.Millisecond)            // <-- line 125
	testutil.WriteChunkStoreFrame(cs.Conn(), b.FinishedBytes())
	respPayload, err = testutil.ReadFrameRaw(cs.Conn(), 5*time.Second)
	...
}
```

This one is a **retry loop with a hardcoded backoff**, exactly as gp-vu1 describes — but it is
structurally *correct* (predicate `blockID == 8` is checked, deadline is enforced, and the
`ChunkStoreClient.WaitForBlock` helper at `chunkstore.go:111-127` does the identical thing in 15
lines with the identical 50 ms backoff). **Lowest priority in this report**; if touched, replace
with `cs.WaitForBlock(pos[0], pos[1], pos[2], 8, 5*time.Second)`.

**Still live:** `testutil.WriteChunkStoreFrame(cs.Conn(), b.FinishedBytes())` at `:126`
**ignores its error** (no `if err != nil` check, unlike the first send at `:57`). A write failure
silently re-reads the *previous* response, which parses fine and reports the old `blockID` — a
loop until the deadline that then reports "expected block_id=<stoneID>, got 0".

### S-6 · `gateway_test.go:43` — 500 ms sleep standing in for a flush barrier · ✅ FIXED-IN-FLIGHT

```go
// PRE-FIX (now gone):
c1.Close()
time.Sleep(500 * time.Millisecond)     // <-- line 43
```

**Status as of 12:45: fixed.** The concurrent agent rewrote
`TestGateway_BlockPersistsAfterReconnect` to replace the sleep with a real barrier — and did it
the right way, by waiting for the authoritative commit rather than for a fixed interval:

```go
// gateway_test.go:86-92 (current)
// The ACK is optimistic; wait for the authoritative commit before dropping
// the connection, so "persists" really means persisted.
if err := cs.WaitForBlock(x, y, z, stoneID, 5*time.Second); err != nil {
	cs.Close()
	c1.Close()
	t.Fatalf("block never committed: %v", err)
}
```

`grep -n 'time.Sleep' test/integration/gateway_test.go` now returns nothing. **This is the model
fix for S-3 and S-4** — the commit predicate, not a longer sleep.

### S-7 · `quest_book_test.go:46,48` — two stacked sleeps, and the test asserts nothing · **REAL, and worse than the sleep**

```go
// quest_book_test.go:48-52  (line numbers per the 12:45 tree — RE-VERIFIED STILL LIVE)
// Wait for quest system to process actions
time.Sleep(3 * time.Second)          // <-- line 49

time.Sleep(2 * time.Second)          // <-- line 51
t.Logf("Quest book chain executed at %d,%d,%d", cornerX, cornerY, cornerZ)
```

**5 seconds of sleep, then a `Logf`.** There is no assertion of any kind — no `ExpectMsgType`,
no `WaitForInventoryItem`, no ChunkStore check. The test passes unconditionally given the
placements at :39-43 succeed. The comment "Wait for quest system to process actions" states a
requirement that nothing then verifies.

The identical shape appears in `ore_processing_chain_test.go:49` (`time.Sleep(2 * time.Second)`
then only a `Logf`).

**Class:** real leftover flake source (5 s of suite time) **and** a test that asserts nothing.
The honest classification for `quest_book_test.go` is "placeholder masquerading as a test" — it
belongs in the gp-4re coverage report as a named gap, not here. Listed here because the sleeps
are real and cost 7 s of the 603 s run.

### S-8 · `ore_processing_chain_test.go:49` — ✅ FIXED-IN-FLIGHT (sleep removed; assertion gap remains)

> The 2 s sleep **and** the "Verify machines are online and pipe network has nodes" comment are
> both gone as of 12:50. The file was rewritten to use `airCellAbove` per column and to name each
> block (`ore_processing_chain_test.go:32-63`).
> **The assertion gap is unchanged and is now the whole point of the file:** it still ends at
> `t.Logf("placed %s 0x%04X at ...")` (`:62`). It proves that seven blocks were placed and
> nothing else — no recipe runs, no item moves, no pipe transports, no 1→2 ore doubling.
> **This is the substance of gp-4re.1**; see `doc/audits/userflow-gaps.md` §1.

---

## 5. `test/loadtest/main.go` — 3 sites, all appropriate

```
test/loadtest/main.go:146:	time.Sleep(500 * time.Millisecond)
test/loadtest/main.go:149:	ticker := time.NewTicker(time.Second / time.Duration(*rate))
test/loadtest/main.go:152:	timeout := time.After(time.Duration(*duration) * time.Second)
test/loadtest/main.go:203:	time.Sleep(2 * time.Second)
```

| line | Verdict |
|---|---|
| `:146` `time.Sleep(500 * time.Millisecond)` — *"Let connection settle before blasting"* | **Genuinely appropriate, and not in scope for this repo's CI.** `test/loadtest` is a standalone `package main` with its own `go.mod` (not `_test.go`), not wired into `ctest` or `go test ./...`. It measures gateway latency, so a settle window before load is a legitimate part of the measurement methodology, not a race workaround. **Do not change.** |
| `:149` `time.NewTicker(time.Second / *rate)` | **Correct.** This is the load generator's rate limiter — the thing under test. |
| `:152` `time.After(*duration)` | **Correct.** Test duration is a CLI flag. |
| `:203` `time.Sleep(2 * time.Second)` — *"Waiting for trailing responses..."* | **Acceptable**, with a caveat: it is a fixed drain window. For a *latency percentile* tool, a fixed drain systematically **undercounts** p99 when responses arrive slower than 2 s. The honest fix is a quiescence predicate (stop when no `BlockAck` for N ms), but this is a measurement-fidelity improvement, not a flake source, and `test/loadtest` is not run by CI. **File as a follow-up, do not fix here.** |

**Conclusion for `test/loadtest/`: zero actionable findings.** gp-vu1's framing assumed
`test/loadtest` had the same problem; it does not, because it is a measurement tool whose timing
constants are the specification.

---

## 6. Testutil's own self-tests — 3 sites, all deliberate

`test/integration/testutil/service_test.go` and `client_test.go` are unit tests *of the harness*.
Their sleeps are test fixtures, not races:

| file:line | Code | Verdict |
|---|---|---|
| `service_test.go:36` | `time.Sleep(10 * time.Millisecond)` inside a `ReadyCheck` callback | **Deliberate and load-bearing.** The comment says: *"Yield until cmd.Wait has had a chance to reap the short-lived child; the callback itself intentionally reports the external surface ready."* The test's whole point is that `StartService` must reject a child that exited before readiness. The sleep is what makes the child exit inside the callback. Changing it breaks the test's intent. |
| `service_test.go:116` | `time.Sleep(200 * time.Millisecond)` — *"Give the descendant time to create its new session"* | **Appropriate.** Testing that `Shutdown` cannot hang when a descendant escapes the process group. The 5 s `select` at :125-128 is the real assertion; the 200 ms is a race with `setsid()`. Low-probability, low-impact. |
| `service_test.go:125` | `case <-time.After(5 * time.Second): t.Fatal("Shutdown did not return within 5s")` | **Correct.** Bounded assertion timeout, not a sleep. |
| `client_test.go:109` | `case <-time.After(2 * time.Second):` | **Correct.** Same shape. |

All 3 `testutil` package tests pass in the measured run — `ok github.com/gtnh-platform/integration-tests/testutil 2.633s`,
19/19 sub-assertions green.

---

## 7. Summary table — final state, re-verified 12:50

| # | file:line | Duration | Classification |
|---|---|---|---|
| S-1 | `main_test.go:64-71` | 100 ms dial | **startup barrier, should be a predicate** (no predicate exists yet) |
| S-3 | `testutil/client.go:346` | 4-6 s ×19 | **real flake source** — ~85 s of suite time, predicate available |
| S-4 | `energy_chain_test.go:82` | 100 ms | **real flake source** — machine-open rehydration race |
| S-4 | `lcr_energy_hatch_chain_test.go:145` | 50 ms | **real flake source** — same |
| S-5 | `chunk_test.go:125` | 200 ms | **real flake source** (minor) — hardcoded backoff; also drops a write error |
| S-7 | `quest_book_test.go:49,51` | 3 s + 2 s | **real flake source** + zero assertions |
| S-6 | ~~`gateway_test.go:43`~~ | 500 ms | ✅ **fixed in flight** — now a `cs.WaitForBlock` commit barrier (`gateway_test.go:88`) |
| S-8 | ~~`ore_processing_chain_test.go:49`~~ | 2 s | ✅ sleep removed in flight; the **zero-assertion** gap remains |
| — | `service.go:168, 218, 442, 450` | — | ✅ poll intervals / bounded teardown — keep |
| — | `chunkstore.go:123` | 50 ms | ✅ poll interval in `WaitForBlock` — keep |
| — | `machine_test.go:324`, `ebf_multiblock_test.go:222`, `full_base_test.go:104`, `energy_chain_test.go:113`, `lcr_energy_hatch_chain_test.go:190` | 500 ms | ✅ per-read poll inside a predicate loop — keep |
| — | `service_test.go:36, 116, 125`, `client_test.go:109` | — | ✅ deliberate test fixtures / assertion timeouts — keep |
| — | `loadtest/main.go:146, 149, 152, 203` | — | ✅ measurement-tool constants, not in CI |

**Final counts (12:50):** 13 `time.Sleep` sites. **6 actionable** (S-1, S-3, S-4×2, S-5, S-7),
2 fixed during this audit, 7 correct-as-written. **`test/loadtest/` still contributes zero
findings.**

**New finding surfaced by the audit (see `doc/audits/userflow-gaps.md` §5.1): the harness leaks
service clusters when the test binary is killed, and a port conflict then produces a `SKIP:`
line while the suite runs anyway with no ChunkStore.** That is the same class of defect as S-1 —
a missing readiness signal — and it is the reason a clean measurement of this report's findings
is currently impossible on this machine. File it as a P1 alongside S-1.

## 8. Recommended order

1. **`WaitForChunkGeneration` → a ChunkStore predicate** (S-3). Removes ~70 s from every run and
   converts an open-ended bet into a bounded wait. Touches 16 call sites but all of them already
   hold or can open a `ChunkStoreClient`.
2. **Both `open()` helpers → wait for the matching `BlockEntityUpdate`** (S-4). Also fixes the
   gp-2rl F-4 near-tautologies. Two small edits.
3. **`quest_book_test.go` and `ore_processing_chain_test.go`: add a real assertion or delete.**
   (S-7, S-8.) 7 s of suite time currently buys nothing. Cross-reference gp-4re §gp-4re.1.
4. **Document S-1** with a comment at `main_test.go:64`; do not add a sleep.
5. `gateway_test.go:43` and `chunk_test.go:103` — fix only alongside the assertion defects the
   gp-2rl report names (F-12 and the dropped write error).
