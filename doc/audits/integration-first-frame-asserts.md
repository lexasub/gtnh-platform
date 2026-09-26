# gp-2rl — Integration tests that assert on a first frame without correlating

**Audit date:** 2026-09-26
**Scope:** `test/integration/**` (read-only, no source file modified)
**Tree state:** `926521cc` + dirty worktree. **Re-verified against the tree at 12:45**, after a
concurrent agent (gp-dinp/gp-c56) landed edits to 11 of the 22 test files. Findings F-1, F-2, F-10
(partial) and F-12 are **already fixed by that agent** and are retained below marked
`✅ FIXED-IN-FLIGHT` with a note on what they now do, because the fix pattern is what the
remaining findings must follow. All other findings are live and re-verified.

Line numbers below are as of 12:45. Files the concurrent agent has not touched
(`energy_chain_test.go` mtime 12:21, `pipe_wrench_test.go`, `chemical_reactor_side_test.go`,
`single_block_side_config_test.go`, `ebf_multiblock_test.go`, `lcr_energy_hatch_chain_test.go`)
have stable line numbers.

---

## 0. The bug class, restated from the code

`ExpectMsgType` is the uncorrelated primitive:

```go
// test/integration/testutil/client.go:187-209
func (c *GatewayClient) ExpectMsgType(expected uint8, timeout time.Duration) ([]byte, error) {
	...
	for time.Now().Before(deadline) {
		msgType, data, err := c.ReadCtrl(remaining)
		if err != nil { return nil, err }
		if msgType == expected { return data, nil }
		// Unexpected type — skip and retry (push notifications are interleaved)
	}
	return nil, fmt.Errorf("timeout waiting for msg_type %d", expected)
}
```

It matches on **message type alone**. Every type it can match is an *uncorrelated router push*
forwarded verbatim to the ctrl socket by Gateway — none of them carry a request-id header the
harness can key on by construction:

| Test asserts | Gateway forward (all via `send_to_client_ctrl_raw`, `src/apps/gateway/gateway.cpp`) |
|---|---|
| `MsgInventoryUpdate` (6) | `gateway.cpp:440-441` — topic `player.inventory.update` |
| `MsgBlockEntityUpdate` (8) | `gateway.cpp:442-443` — topic `world.block_entity.update` |
| `MsgCraftResponse` (10) | `gateway.cpp:454-455` — topic `sim.craft.response` |
| `MsgPipeContentsResp` (49) | `gateway.cpp:466-470` — topic `pipe.contents.response` |
| `MsgSetMachineSlotResp` (16) | `gateway.cpp:474-475` — topic `player.machine.slot.response` |
| `MsgToolActionResp` (14) | `gateway.cpp:476-477` — topic `player.tool.action.response` |
| `MsgBlockAck` (5) | `gateway.cpp:436-437` — topic `player.actions.ack` |
| `MsgGameModeChange` (30) | echo of the client's own request |

Two of these payloads *do* carry enough to correlate on (`BlockAck.request_id`,
`InventoryUpdate.player_id`) — which is exactly why `WaitForBlockAck` and `WaitForInventoryItem`
exist. The finding below is that most call sites still use the type-only primitive even when
the correlation field is present in the schema and already has a helper.

**The seed defect, for reference:** `testutil.WaitForInventoryItem` (`client.go:219-260`) exists
precisely because `ExpectMsgType` accepted the *first* `InventoryUpdate` frame, which is not the
snapshot that proves the grant. It is used at only 3 non-test sites today
(`inventory_test.go:87`, `inventory_test.go:191`, `machine_test.go:171`,
`questbook_wire_tracer_test.go:56`).

---

## 1. Premise check: "NO test currently does a chunk prime"

**This premise is FALSE.** 19 of the 21 test files call `RequestChunk` + `WaitForChunkGeneration`
before any CAS placement. Verified 12:45:

```
$ grep -rc 'RequestChunk' test/integration/*_test.go | grep -v ':0' | wc -l
19
```

`$ grep -rn 'RequestChunk' test/integration/*.go` — full site list (22 calls):

```
base_build_test.go:23            energy_chain_test.go:25       machine_test.go:259
block_test.go:70 (primeChunk)    full_base_test.go:29          ore_processing_chain_test.go:22
chemical_reactor_side_test.go:32 gateway_test.go:61            pipe_wrench_test.go:31
chunk_test.go:28                 inventory_test.go:35, :151    quest_book_all_crafts_full_test.go:25
crafting_test.go:105             lcr_energy_hatch_chain_test.go:48  quest_book_full_craft_test.go:37
ebf_multiblock_test.go:88, :167  quest_book_player_progress_test.go:34
                                  quest_book_test.go:22          single_block_side_config_test.go:36
```

Only `stress_test.go` places blocks with no prime — see §3.

The `primeChunk` helper (`block_test.go:68-74`) exists and is used by the three `block_test.go`
CAS tests, but 16 other call sites inline the two-line pattern instead of calling it. That is a
DRY defect, not a correctness defect — and it is the most likely reason the `stress_test.go` gap
went unnoticed.

---

## 2. Findings — bare `ExpectMsgType` on a type-only match

Ranked by probability the test passes for the wrong reason.

### F-1 · `machine_test.go` `TestMachine_SlotTransfer` — first `SetMachineSlotResp` · ✅ FIXED-IN-FLIGHT

> **Status as of 12:45:** fixed. The concurrent agent added an `invSnapshot` helper
> (`machine_test.go:17-85`) and routed both subtests through
> `invSnapshot.sendMachineSlot` (`machine_test.go:195, 210, 218`), which buffers
> `InventoryUpdate` frames instead of discarding them, so the `WaitForInventoryItem` evidence
> can no longer be consumed by a response wait.
> **However the residual defect remains:** `sendMachineSlot` (`machine_test.go:54-63`) still
> matches `SetMachineSlotResp` **by type alone** — `if msgType == testutil.MsgSetMachineSlotResp`
> at `:59`, with no `resp.Pos()` check, even though `SetMachineSlotResp.pos` is
> `required` in the schema. In this specific test all three sends target the *same* furnace, so
> the residual risk is low; it is the *same helper* that F-3/F-5/F-6 need, and it is the wrong
> helper to generalise. Fixing the helper's `pos` check closes F-1, F-2, F-3, F-5 and F-6 at
> once.

Original (pre-fix) code, kept for reference:

```go
// machine_test.go:123-134 (t.Run("PlaceItemInMachineSlot"))
c.SendCtrl(testutil.MsgSetMachineSlot, ... 0 /*input slot*/, cobbleItemID, 1, 0, 255)
data, err := c.ExpectMsgType(testutil.MsgSetMachineSlotResp, 5*time.Second)
if resp := Protocol.GetRootAsSetMachineSlotResp(data, 0); resp == nil || !resp.Success() { ... }
```

**Why a wrong frame can satisfy it:** the enclosing test is two subtests that each send a
`SetMachineSlotReq` and each expect a `Resp`, on **one shared connection** (`machine_test.go:85-89`
dials once, `t.Run` at :120 and :137). `ExpectMsgType` keeps any *unmatched* type out of the way
but has no memory, so the `Resp` from subtest 1 is already consumed by subtest 1 — however
`machine_test.go:171` (`WaitForInventoryItem`) and the `BlockEntityUpdate` stream from
`machine_test.go:245`'s sibling in the other test both push interleaved frames onto the same
socket, and `SetMachineSlotResp` is *also* emitted by any other actor's slot write. The schema
has `pos` (`src/protocol/machine_state.fbs`, `SetMachineSlotResp { pos: Vec3i (required); ... }`)
so the frame **can** be correlated; the test does not.
**Verdict:** correlated data available, not used. Fix = assert `resp.Pos() == (x,y,z)`.

### F-2 · `machine_test.go` refill + extract pair · ✅ FIXED-IN-FLIGHT (same `sendMachineSlot` caveat as F-1)

> Now `machine_test.go:210` (refill) and `:218` (extract) via `sendMachineSlot`. The
> `success` checks at `:212` and `:220` are now non-vacuous, and the final assertion
> `snap.awaitItem(stickID, 3, ...)` at `:224` is correlated. Residual: the type-only match, as
> in F-1.

Original (pre-fix) code:

```go
// :150  refill response (stickID 3)
// :163  extract response (item_id=0, count=0, player_slot=5)
data, err := c.ExpectMsgType(testutil.MsgSetMachineSlotResp, 5*time.Second)
```

Same defect as F-1, and worse: these are three `SetMachineSlotReq` sends in a row on one socket.
If the second `ExpectMsgType` at :163 consumes the response to a *retry* of :145, the extract path
is never proven. The only thing that catches this today is the `WaitForInventoryItem(playerID,
stickID, 3, …)` at :171 — which is a genuine correlated assertion, so the test as a whole is not
vacuous, but the intermediate `success` checks at :152 and :167 are.

### F-3 · `machine_test.go:256` — `insert()` closure in the generator→furnace chain · **MEDIUM**

```go
// machine_test.go:301-314  (line numbers per the 12:45 tree)
insert := func(what string, x, z int32, slot uint16, item uint16, count uint8) {
	c.SendCtrl(testutil.MsgSetMachineSlot, ... )
	data, err := c.ExpectMsgType(testutil.MsgSetMachineSlotResp, 5*time.Second)
	if resp := Protocol.GetRootAsSetMachineSlotResp(data, 0); resp == nil || !resp.Success() {
		t.Fatalf("server rejected inserting %s", what)
	}
}
insert("coal into generator", ...)   // :315
insert("iron ore into furnace", ...) // :316
```

**Why a wrong frame can satisfy it:** the closure fires twice against two *different* machines
(generator at `genCell`, furnace at `furnacePos`). The message names the intent in `what` but the
assertion only checks `success`, so a response for the generator can satisfy the furnace's insert
and vice versa. The payload carries `pos`, so the correlation is available and unused.

### F-4 · `machine_test.go:245` and `ebf_multiblock_test.go:200` and `full_base_test.go:83` — `BlockEntityUpdate` on machine open · **HIGH**

```go
// machine_test.go:288-299  (line numbers per the 12:45 tree)
for _, m := range []struct{ x, z int32; id uint16 }{{furnacePos[0], furnacePos[2], heatFurnaceID},
                                                    {genCell[0], genCell[2], heatGeneratorID}} {
	c.SendCtrl(testutil.MsgMachineOpenReq, testutil.BuildContainerOpenReq(playerID, m.x, y, m.z))
	if _, err := c.ExpectMsgType(testutil.MsgBlockEntityUpdate, 5*time.Second); err != nil {
		t.Fatalf("open update for 0x%04X: %v", m.id, err)
	}
}
```

**⚠ STILL LIVE (re-verified 12:45).** Four sites, all `if _, err := c.ExpectMsgType(
testutil.MsgBlockEntityUpdate, 5*time.Second)` immediately after a `MsgMachineOpenReq`:
`machine_test.go:296`, `ebf_multiblock_test.go:200`, `full_base_test.go:86`,
`chemical_reactor_side_test.go:63`. The concurrent agent did not touch these.

**This is the most dangerous pattern in the suite.** `BlockEntityUpdate` is a
**broadcast**: Gateway forwards `world.block_entity.update` from *any* machine
(`gateway.cpp:442-443`), and SimCore publishes machine state continuously at tick rate. The
payload carries `pos: Vec3i (required)` (`src/protocol/machine_state.fbs`), and the very next
loop in the same file (:272-293) *does* correlate on `pos` — proving the author knew the field
exists. The `:237-248` open-wait does not.

**A wrong frame satisfies it trivially:** SimCore's 20 Hz state publication means a
`BlockEntityUpdate` for the *other* machine (or for any machine from a previous test in the same
process-scoped cluster) is almost always already in the socket buffer. This assertion proves
only "a machine somewhere published state", i.e. it is a near-tautology.
**The consequence is concrete:** the open-wait at `machine_test.go:296` is what makes the
subsequent inserts reliable, and the same pattern at `ebf_multiblock_test.go:200`,
`full_base_test.go:86` and `chemical_reactor_side_test.go:63` means those tests' `open()` steps
may silently no-op while still passing.

**Fix:** a `WaitForBlockEntityUpdate(playerID, x, y, z, timeout)` helper filtering on
`update.Pos()`. The polling loops at `machine_test.go:272`, `ebf_multiblock_test.go:221`,
`lcr_energy_hatch_chain_test.go:188`, `energy_chain_test.go:111` already contain the correct
filter inline — the helper is a copy of existing correct code.

### F-5 · `lcr_energy_hatch_chain_test.go:166` — `put()` closure, 9 invocations · **HIGH**

```go
// lcr_energy_hatch_chain_test.go:161-174
put := func(x, y, z int32, slot uint16, item uint16, count byte) {
	c.SendCtrl(testutil.MsgSetMachineSlot, ...)
	respData, err := c.ExpectMsgType(testutil.MsgSetMachineSlotResp, 5*time.Second)
	resp := Protocol.GetRootAsSetMachineSlotResp(respData, 0)
	if resp == nil || !resp.Success() { t.Fatalf("insert item %d rejected: %s", item, ...) }
}
```

Called 9 times (`:176-179`) across 4 distinct branches (z = 680/686/692/698) plus the LCR
controller. Same class as F-3, maximal blast radius. `resp.Pos()` is available and unused.

### F-6 · `energy_chain_test.go:98` — `put()` closure, 3 invocations · **MEDIUM**

```go
// energy_chain_test.go:93-101
put := func(x, y, z int32, slot uint16, item uint16, count uint8) {
	c.SendCtrl(testutil.MsgSetMachineSlot, ...)
	if _, err := c.ExpectMsgType(testutil.MsgSetMachineSlotResp, 5*time.Second); err != nil { ... }
}
```

Note this one is *weaker* than F-5: it does not even check `resp.Success()`. It only asserts a
frame of that type arrived. Three distinct machines (generator, battery, compressor) are involved.

### F-7 · `chemical_reactor_side_test.go:63` — `BlockEntityUpdate` after open · **HIGH**

```go
// chemical_reactor_side_test.go:60-65
c.SendCtrl(testutil.MsgMachineOpenReq, testutil.BuildContainerOpenReq(playerID, baseX, baseY, baseZ))
if _, err := c.ExpectMsgType(testutil.MsgBlockEntityUpdate, 5*time.Second); err != nil {
	t.Fatalf("no block entity update")
}
```

Same broadcast defect as F-4, and it is the *only* verification in the test that the wrench side
config took effect (see the log line at :66, "Side config verified for chemical reactor"). The
assertion therefore verifies nothing about side config. `PipeContentsResp` on the neighbouring
`pipe_wrench_test.go:72` would be the real proof.

### F-8 · `single_block_side_config_test.go:100` and `pipe_wrench_test.go:72` — `PipeContentsResp` · **MEDIUM**

```go
// single_block_side_config_test.go:97-103
c.SendCtrl(testutil.MsgPipeContentsReq, testutil.BuildPipeContentsReq(playerID, pipeX, pipeY, pipeZ))
if _, err := c.ExpectMsgType(testutil.MsgPipeContentsResp, 5*time.Second); err != nil {
	t.Fatalf("pipe contents resp timeout")
}
t.Logf("Pipe contents verified after side config")
```

```go
// pipe_wrench_test.go:68-75  (note: raw literals 48/49 instead of the named constants)
c.SendCtrl(48, testutil.BuildPipeContentsReq(playerID, pipeX, pipeY, pipeZ))
if _, err := c.ExpectMsgType(49, 5*time.Second); err != nil { ... }
```

`PipeContentsResp` **does** carry `player_id` and `pos` (`src/protocol/pipe_network.fbs`:
`PipeContentsResp { player_id: uint64; pos: Vec3i (required); found: bool = false; ... }`).
Neither is checked, and `found`/`fluid_id`/`amount` are not checked either — so the "verified"
log lines are unearned. These are the only two `PipeContentsResp` sites and both are wrong-frame
prone *and* assertion-empty.

### F-9 · `crafting_test.go:191` — `CraftResponse` · **LOW-MEDIUM**

```go
// crafting_test.go:181-196 (benchSession.craft)
c.SendCtrl(testutil.MsgCraftRequest, testutil.BuildCraftRequest(b.playerID, p[0], p[1], p[2], emptyGrid))
data, err := b.c.ExpectMsgType(testutil.MsgCraftResponse, 5*time.Second)
return Protocol.GetRootAsCraftResponse(data, 0)
```

`CraftResponse` (`src/protocol/recipe.fbs`) has `success`, `result`, `error`, `grid` — and
**no player_id or position field at all**. So correlation is impossible at the schema level
without a protocol change. The test partially compensates: the enclosing
`benchSession.wait` (:52-83) does correlate `InventoryUpdate` on `player_id`, and each
`benchSession` gets its own bench position (`crafting_test.go:202-204` documents this
explicitly), so the number of in-flight crafts on one socket is 1.
**This is the strongest-correct use of `ExpectMsgType` in the suite** and is ranked low because
the un-correlation is a protocol limitation, not a harness defect. Note it is still a *shared
socket* risk: `TestCrafting_RequestResponse` and `TestCrafting_ValidInvalid` both dial once
(`crafting_test.go:206-210`, `machine_test.go:24-28`) and each fire two `craft()` calls.

### F-10 · `BlockAck` with **request_id = 0** · PARTIALLY ✅ FIXED-IN-FLIGHT

> **Fixed:** `gateway_test.go:81` and `chunk_test.go:54` now use
> `WaitForBlockAck(reqID, ACCEPTED, …)` with a non-zero `placeReqID`/`reqID`, and
> `gateway_test.go` also primes the chunk (`:61-65`) and waits for the ChunkStore commit
> (`:88`) before dropping the connection.
> **Still live:** all three `stress_test.go` tests. `grep -rn ExpectMsgType stress_test.go`
> still returns `:36`, `:76`, `:121`, all with `request_id = 0` (they use
> `BuildSetBlockAction`, `fb_builders.go:39-41`, which passes an empty
> `SetBlockActionOptions{}`). The `nil`-err bug below is also unfixed.

Original (pre-fix) code:

```go
// gateway_test.go:32-40
fbData := testutil.BuildSetBlockAction(42, pos[0], pos[1], pos[2], 0, 8)   // RequestID unset → 0
c1.SendCtrl(testutil.MsgSetBlockAction, fbData)
if _, err := c1.ExpectMsgType(testutil.MsgBlockAck, 5*time.Second); err != nil { ... }
```

`BuildSetBlockAction` (`fb_builders.go:39-41`) passes `SetBlockActionOptions{}` → `RequestID: 0`.
`BuildBreakBlockAction` (`fb_builders.go:145`) likewise. So these acks carry `request_id = 0`.

**This is not merely "could match the wrong frame" — the server sends *two* acks per placement.**
`CasRunner.cpp:17-20` publishes an optimistic `ACCEPTED` before the CAS, and
`CasRunner.cpp:45-48` publishes `CONFLICT` afterwards on failure. Both arrive on the same
uncorrelated topic (`gateway.cpp:436-437`). A bare `ExpectMsgType(MsgBlockAck)` therefore
consumes the *optimistic* one, and — because these tests don't filter by status — any
`BlockAck` satisfies them.

Concretely:
- `gateway_test.go:37` — asserts only that *an* ack arrived. It does not read the status, does
  not read `block_id`, and does not verify the block persisted anywhere. The test's own log
  message at :55 claims "block persisted through disconnect" but the only evidence is
  `ExpectMsgType(MsgCompressedChunk, …)` at :51 — which fires on **any** chunk push, including
  the spawn chunk the Gateway requests automatically on connect
  (`src/apps/gateway/gateway.cpp:116-131`).
- `chunk_test.go:26` — this one *does* check `ack.Status() == ACCEPTED` (:31), but on an
  uncorrelated frame with `request_id = 0`. Its retry loop (:78-113) then re-polls ChunkStore
  directly, so the real assertion is the `blockID != 8` check at :115 — that part is sound.
- `stress_test.go:34` — `TestStress_ConcurrentBlockPlacement`: checks status
  `ACCEPTED` (:40) on a `request_id = 0` ack from 5 concurrent clients. **The status check at
  :40 pushes `err` onto the channel — and `err` is `nil` at that point** (`stress_test.go:34`
  assigned `err` from the `ExpectMsgType` call, which returned no error). So a
  `REJECTED`/`CONFLICT` ack is silently swallowed. This is a live bug independent of correlation.
- `stress_test.go:74` — inside a 10 s loop with a 1 s per-iteration `ExpectMsgType`; the
  `err != nil → continue` at :75-77 means a genuine transport failure is indistinguishable from
  "no ack yet". Ranking by status is done (:79, :83) so this one at least filters, but on an
  uncorrelated `request_id = 0` frame.
- `stress_test.go:119` — `TestStress_RapidFireBlocks`: 10 placements, then 10 bare
  `ExpectMsgType(MsgBlockAck)` calls. All 10 have `request_id = 0`
  (`fb_builders.go:39`). The loop then checks each `ack.Status() == ACCEPTED`. **The 10
  placements all share one request id of 0, so the i-th ack cannot be tied to the i-th
  placement** — but since every assertion is identical (`ACCEPTED`) the practical risk is a
  duplicate optimistic ack from placement *i+1* satisfying the check for *i*. The count check
  (`t.Fatalf("expect ack %d")` at :121) is what actually catches a short read.

**Fix for all of the above:** use `WaitForBlockAck(reqID, status, timeout)` — it filters on both
`request_id` **and** status (`client.go:328-331`) — and give each placement a distinct non-zero
`RequestID` via `BuildPlaceBlockActionWithOptions`.

### F-11 · `questbook_wire_tracer_test.go:41` — `GameModeChange` echo · **LOW (correct)**

```go
// questbook_wire_tracer_test.go:37-48
c.SendCtrl(testutil.MsgGameModeChange, testutil.BuildGameModeChange(playerID, Protocol.GameModeCREATIVE))
modeData, err := c.ExpectMsgType(testutil.MsgGameModeChange, 5*time.Second)
mode := Protocol.GetRootAsGameModeChange(modeData, 0)
if mode.PlayerId() != playerID || mode.NewMode() != Protocol.GameModeCREATIVE { t.Fatalf(...) }
```

**Listed because it is the counter-example that proves the rule.** The type-only `ExpectMsgType`
is *fine* here because the very next lines correlate on `player_id` **and** the mode value, and
because this is the first frame on a fresh connection. This is what a correct
type-only assertion looks like. It is also the first assertion on the connection, so nothing can
precede it.

### F-12 · `CompressedChunk` on connect · ✅ FIXED-IN-FLIGHT (port move), payload still unread

> **Fixed:** both sites now dial the **bulk** port, which is the contractually correct channel
> (`gateway.cpp:387-393`, "chunk data is large" → `world.chunk.loaded.compressed` goes only to
> bulk; ctrl drops it with "compressed chunk dropped, no bulk client"). See
> `gateway_test.go:27-37` and `:104-114`. The rationale is documented in a comment at
> `gateway_test.go:11-16`.
> **Still live:** neither decodes the payload, so both remain assertions that "a chunk push
> exists". `TestGateway_BlockPersistsAfterReconnect` is materially improved — it now waits for
> the authoritative ChunkStore commit at `gateway_test.go:88` before reconnecting — so the
> *persistence* claim is now backed, but the reconnect leg still only proves a chunk was sent.

Original (pre-fix) code:

```go
// gateway_test.go:10-22
data, err := c.ExpectMsgType(testutil.MsgCompressedChunk, 5*time.Second)
t.Logf("Received CompressedChunk (%d bytes) on connect", len(data))
```

```go
// gateway_test.go:45-55 (reconnect)
_, err = c2.ExpectMsgType(testutil.MsgCompressedChunk, 5*time.Second)
t.Logf("Reconnected and received CompressedChunk — block persisted through disconnect")
```

Both are "first frame on a fresh connection", so an *earlier unrelated frame of the same type*
is impossible — that part is sound. But **neither reads the payload**. The chunk data is never
decoded, so `TestGateway_BlockPersistsAfterReconnect` does not test persistence at all: it
asserts "a chunk push exists", which is exactly the tautology gp-dinp flags for
`TestGateway_ReceivesChunkDataOnConnect`. Both are in the gp-dinp 9-failure set.

---

## 3. Missing chunk prime — re-verified at 12:45

**Only three test files place blocks with no chunk prime**, down from five: the concurrent
agent added one to `chunk_test.go` (`:28`) and to `gateway_test.go` (`:61`).

```
$ grep -rLn 'RequestChunk' test/integration/*_test.go
test/integration/main_test.go               # harness, not a test — expected
test/integration/questbook_wire_tracer_test.go  # places no blocks — expected
test/integration/stress_test.go             # REAL GAP
```

| Test | file:line | Positions | Verdict |
|---|---|---|---|
| `TestStress_ConcurrentBlockPlacement` | `stress_test.go:28-30` | x = 600..604, y = 120, z = 600 | **REAL GAP** |
| `TestStress_CASRace` | `stress_test.go:56,69` | (650,120,650) | **REAL GAP** |
| `TestStress_RapidFireBlocks` | `stress_test.go:111-112` | x = 700..709, y = 120, z = 700 | **REAL GAP** |

All three place via `BuildSetBlockAction` with no `airCellAbove` and no prime, at y=120, far from
any spawn position. Per `inventory_test.go:32-34`: "Gateway's automatic CHUNK_REQUEST only covers
the player's saved spawn position, so a placement this far away must generate the chunk first or
the CAS loses the world-gen race and comes back CONFLICT."

**Recommended minimal fix:** call the existing `primeChunk` helper (`block_test.go:61-67`) at the
top of all three. Note none of the three are in gp-dinp's 9-failure set, so this is not currently
masking anything — it is latent.

**DRY defect worth noting:** 16 of the 19 prime sites inline the two-line
`RequestChunk` + `WaitForChunkGeneration` pattern instead of calling the `primeChunk` helper
that `block_test.go:61-67` already provides. Not a correctness issue; it is why the helper is
easy to forget (as it was in the three stress tests).

## 4. Fixed timeout where the real requirement is a predicate

| file:line | Code | Problem |
|---|---|---|
| `testutil/client.go:345-347` | `func (c *GatewayClient) WaitForChunkGeneration(timeout) { time.Sleep(timeout) }` | **The whole function is a sleep.** Its own doc comment says "The chunk protocol has no request ACK" — but `ChunkStoreClient.WaitForBlock` (`chunkstore.go:111-127`) *is* a predicate against the authoritative store and would be the correct barrier. As written, every chunk prime in the suite costs 4 s of pure sleep and still can under-wait. See gp-vu1 report §3. |
| `testutil/client.go:199` | `ReadCtrl(remaining)` inside `ExpectMsgType` | The whole primitive: a timeout where the requirement is a predicate. |
| `machine_test.go:270` | `deadline := time.Now().Add(40 * time.Second)` | 40 s fixed ceiling on a 20 Hz tick loop. Legitimately a bounded wait (the comment at :267-269 says so), but 40 s with no progress logging means a slow machine and a dead machine are indistinguishable. |
| `ebf_multiblock_test.go:215` | `time.Now().Add(30 * time.Second)` | Same, for the EBF smelt. |
| `full_base_test.go:98` | `time.Now().Add(45 * time.Second)` | Same, for the EBF smelt in the full base. |
| `energy_chain_test.go:110` | `time.Now().Add(35 * time.Second)` | Same, for the 4-way thermal chain. |
| `lcr_energy_hatch_chain_test.go:187` | `time.Now().Add(60 * time.Second)` | Same, 60 s for 4 branches + LCR. |
| `ebf_multiblock_test.go:101,181` | `ExpectMultiblockEvent(5s)` / `(8s)` | Bounded wait for a lifecycle event; reasonable. `ExpectMultiblockEvent` does decode and type-check (`client.go:359-373`) and the callers then check `anchor` and `MbType` — **this is correct usage.** |
| `ebf_multiblock_test.go:128` | `ExpectMultiblockEvent(5 * time.Second)` for the *destroyed* event | Correct usage, but note `ExpectMsgType` short-circuits on `pendingMultiblockEvent` (`client.go:188-192`) which `WaitForBlockAck` populates (`client.go:321-324`) — the ordering is sound because the break is sent at :117 and the ack awaited at :120 first. |
| `inventory_test.go:191` | `WaitForInventoryItem(playerID, cobblestoneID, 1, 15*time.Second)` | 15 s where 5 s is used elsewhere. Not wrong; just note the asymmetry with `inventory_test.go:87` (`5*time.Second`). |

---

## 5. A positive finding: what is already correct

Do not regress these. They are the model the fixes should follow.

1. **`inventory_test.go:112-133`** — hand-rolled but *correct*: filters on
   `msgType == MsgInventoryUpdate` **and** `update.PlayerId() == playerID` **and**
   `InventoryItemCount(data, cobblestoneID) == 0`. The in-source comment at :75-77 names gp-c56.
2. **`inventory_test.go:87,191`** and **`questbook_wire_tracer_test.go:56`** — `WaitForInventoryItem`.
3. **`crafting_test.go:52-83`** (`benchSession.wait`) — retains the last non-matching snapshot in
   `b.last` and re-checks it first (:57-62). This is a better design than `WaitForInventoryItem`,
   which discards non-matching frames entirely. Worth promoting to a shared helper.
4. **`machine_test.go:272-293`, `ebf_multiblock_test.go:221-251`, `full_base_test.go:100-122`,
   `lcr_energy_hatch_chain_test.go:188-242`, `energy_chain_test.go:111-135`** — all five poll
   `BlockEntityUpdate` and filter on `update.Pos()`. The `Pos()` filter that F-4/F-7 need is
   already written five times; the fix is to hoist it into `testutil`.
5. **`testutil/client.go:310-334`** (`WaitForBlockAck`) — filters on `request_id` **and**
   `status`, and parks `MsgMultiblockEvent` frames in `pendingMultiblockEvent` so lifecycle
   events are not lost while waiting for an ack. Used correctly at 9 sites.

---

## 6. Summary table — final state, re-verified 12:45

### Still live (7 findings)

| # | file:line | Assertion | Correlatable? | Passes for wrong reason? |
|---|---|---|---|---|
| F-3 | `machine_test.go:307` | `ExpectMsgType(SetMachineSlotResp)` in `insert()`, 2 different machines | yes (`pos`) | **likely** |
| F-4 | `machine_test.go:296`, `ebf_multiblock_test.go:200`, `full_base_test.go:86` | `ExpectMsgType(BlockEntityUpdate)` after machine open | yes (`pos`) | **almost certain** (broadcast) |
| F-5 | `lcr_energy_hatch_chain_test.go:166` | `ExpectMsgType(SetMachineSlotResp)` in `put()`, 9 different machines | yes (`pos`) | **likely** |
| F-6 | `energy_chain_test.go:98` | `ExpectMsgType(SetMachineSlotResp)` in `put()`, 3 machines, **no `success` check at all** | yes (`pos`) | **likely** |
| F-7 | `chemical_reactor_side_test.go:63` | `ExpectMsgType(BlockEntityUpdate)`, the test's *only* side-config check | yes (`pos`) | **almost certain** + vacuous |
| F-8 | `single_block_side_config_test.go:100`, `pipe_wrench_test.go:72` | `ExpectMsgType(PipeContentsResp)`, payload entirely unchecked | yes (`player_id`,`pos`,`found`,`fluid_id`,`amount`) | **likely** + vacuous |
| F-10a | `stress_test.go:36,76,121` | `ExpectMsgType(BlockAck)` with `request_id = 0` | yes (`request_id`) | **likely**; double-ack makes it certain for ACCEPTED |
| F-12a | `gateway_test.go:37,114` | `ExpectMsgType(CompressedChunk)`, payload never decoded | n/a (correct port now) | no, but **vacuous** |
| F-9 | `crafting_test.go:191` | `ExpectMsgType(CraftResponse)` | **no** — schema has no player_id | low — protocol limit |

### Correct as written — do not touch (2)

| file:line | Why it is correct |
|---|---|
| `questbook_wire_tracer_test.go:41` | Type-only match is fine: the next lines correlate on `player_id` **and** mode, and it is the first frame on a fresh connection. |
| `machine_test.go:59` (`invSnapshot.sendMachineSlot`) | Buffers non-matching `InventoryUpdate` frames rather than discarding them — strictly better than `ExpectMsgType`. Still type-only on the resp; see F-1 note. |

### Fixed by the concurrent gp-dinp/gp-c56 agent during this audit (4)

F-1, F-2 (both now route through `invSnapshot.sendMachineSlot`); F-10 partially (`gateway_test.go:81`,
`chunk_test.go:54` now use `WaitForBlockAck` with non-zero request ids); F-12 partially (both chunk
sites moved to the correct bulk port; `TestGateway_BlockPersistsAfterReconnect` now also waits for
the ChunkStore commit at `gateway_test.go:88`).

**Counts at 12:55 (final):** **21** `ExpectMsgType` call sites remain across **12** test files
(chemical_reactor_side ×2, crafting ×1, ebf_multiblock ×2, energy_chain ×1, full_base ×2,
gateway ×2, lcr ×1, machine ×2, pipe_wrench ×2, questbook_wire_tracer ×1, single_block_side_config ×2,
stress ×3). The starting count was 24. Of the 21 remaining: **16 are fixable today** with no
protocol change; 1 is a protocol limitation (`CraftResponse`); 2 are correct; 2 are vacuous rather
than wrong-frame-prone. The `stress_test.go` sites are re-verified live at 12:55 — all three still
send via `BuildSetBlockAction` (no `RequestID`, no `primeChunk`).

**Plus 2 independent defects, both still live:**
- `stress_test.go:40-44` — the non-ACCEPTED branch pushes `err` onto the channel, and `err` is
  `nil` at that point (it was assigned from the successful `ExpectMsgType` at `:36`). A
  `REJECTED`/`CONFLICT` ack is silently reported as success.
- `testutil/chunkstore.go:157-159` (`PlaceBlockAndWait`) — on `WaitForBlock` failure it
  `requestID++` and retries, so the ids it actually sends drift from the caller's; with
  per-test id bases like `30000+i` (`ebf_multiblock_test.go:96`) a long fixture can collide with
  a neighbouring fixture's ids. Low probability, real.

---

## 7. Recommended fix order

1. **Add `resp.Pos()` to `invSnapshot.sendMachineSlot` (`machine_test.go:59`).** The helper
   already exists and already loops; one condition closes F-1, F-2, F-3 and unblocks a clean
   lift of F-5/F-6 onto it. Cheapest highest-leverage change in the suite.
2. **Hoist the `Pos()` filter into `testutil.WaitForBlockEntityUpdate(x,y,z,timeout)`** and route
   F-4 and F-7 through it. Four currently near-tautological assertions become real, and it
   deletes 5 copies of the same inline poll loop.
3. **Check `PipeContentsResp.player_id`/`pos`/`found`** at F-8's two sites; both currently log
   "verified" for an unverified frame.
4. **Fix `stress_test.go:40-44`** (the `nil` err) and give the three stress tests non-zero
   `RequestID`s + a `primeChunk` call. One test file, three defects.
5. **`CraftResponse`**: raise a protocol bead — add `player_id` to the table so the last
   uncorrelatable push type can be fixed properly.

Do not regress: `inventory_test.go:112-133`, `crafting_test.go:52-83` (`benchSession.wait`, which
retains the last non-matching snapshot), the five `update.Pos()`-filtered poll loops, and
`testutil/client.go:310-334` (`WaitForBlockAck`).
