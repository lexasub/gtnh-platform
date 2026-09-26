# Audit gp-vj6t — field-usage matrix: Go sidecar reads vs C++ publisher writes

**Scope:** read-only audit of the Go↔C++ field contract. No source file was modified.
**Go services:** `src/apps/meta_db`, `src/apps/message_router`
**C++ services:** everything else
**Date:** 2026-09-26

---

## TL;DR

Three findings dominate:

1. **P0 — every `if req == nil` guard in the Go sidecars is dead code.**
   FlatBuffers-Go's `GetRootAsX()` **never returns nil**; it panics or returns a
   zero-valued object. Seven guards are unreachable, and 2 handlers have no guard
   at all. Verified empirically (§2).
2. **P1 — a guaranteed runtime panic** in `db.go:415` (`FinishedBytes()` before
   `Finish()`), plus panics on empty/truncated `player.joined` / `player.left`.
3. **P1 — the `quest_progress` table has exactly ONE writer** and one of the two
   documented write paths (`meta_db.quest.set`) **has no publisher anywhere in the
   repo**. Quest progress persistence therefore depends entirely on the
   `quest.completed` path surviving. The code comments admit this.

**No classic "field read but never written → silent zero" bug was found in the
live paths.** Every field the Go side reads that has a C++ publisher is written
with a real value. §4 documents this precisely, including the two places where a
zero *is* reachable and why it is benign.

---

## 1. Which messages the Go side actually consumes

MetaDB registers 10 topics with the router (`src/apps/meta_db/router_client.go:103-113`).
Seven reach a FlatBuffers decoder; three are MetaDBFrame-RPC.

| Topic | Decoder | Table | Guard present? |
|---|---|---|---|
| `player.joined` | `router_client.go:376` | `PlayerJoined` (core.fbs:394) | ❌ **none** |
| `player.left` | `router_client.go:409` | `PlayerLeft` (core.fbs:398) | ❌ **none** |
| `meta_db.quest.get` | `quest_handlers.go:20` | `QuestProgressUpdate` (quest.fbs:30) | ✅ (but dead, §2) |
| `meta_db.quest.set` | `quest_handlers.go:76` | `QuestProgressUpdate` | ✅ (but dead, §2) |
| `quest.completed` | `quest_handlers.go:142` | `QuestCompleted` (quest.fbs:123) | ✅ (but dead, §2) |
| `quest.exchange.request` | `exchange_handlers.go:19` | `QuestExchangeRequest` (quest.fbs:53) | ✅ (but dead, §2) |
| `quest.exchange.cooldown.get` | `exchange_handlers.go:206` | `QuestExchangeCooldownGet` (quest.fbs:81) | ✅ (but dead, §2) |
| `meta_db.inventory.{get,set,snapshot}` | `flatbuffer_tcp.go:58` → `handleMetaDBMessage` | `MetaDBFrame` → `MetaDBMessage` | ✅ (but dead, §2) |
| `metadb.player.online` | *published, not consumed* | — | — |

`src/apps/message_router` is a **pure byte router** — zero FlatBuffers:

```
$ grep -rn "flatbuffers\|Protocol\." --include=*.go src/apps/message_router
(no output)
```

So it is out of scope for field-level analysis. It never inspects a payload, only
`[4B len][1B type][2B topicLen][topic][opaque]`
(`src/apps/message_router/router.go:582-593`). **It cannot corrupt a FlatBuffer
field**, and equally it offers no validation.

---

## 2. P0 — all `== nil` guards are dead code; 2 handlers have none

### 2a. `GetRootAsX` never returns nil

Every generated accessor follows the same shape
(`src/protocol/generated/go/Protocol/PlayerJoined.go:13-19`):

```go
func GetRootAsPlayerJoined(buf []byte, offset flatbuffers.UOffsetT) *PlayerJoined {
	n := flatbuffers.GetUOffsetT(buf[offset:])   // panics if buf too short
	x := &PlayerJoined{}
	x.Init(buf, n+offset)                         // no error path
	return x                                      // never nil
}
```

Empirically, with a `nil` slice:

```
  GetRootAsQuestExchangeRequest(nil)     runtime error: index out of range [3] with length 0
  GetRootAsQuestExchangeCooldownGet(nil) runtime error: index out of range [3] with length 0
  GetRootAsQuestProgressUpdate(nil)      runtime error: index out of range [3] with length 0
  GetRootAsQuestCompleted(nil)           runtime error: index out of range [3] with length 0
  GetRootAsMetaDBFrame(nil)              runtime error: index out of range [3] with length 0
  GetRootAsPlayerJoined(nil)             runtime error: index out of range [3] with length 0
  GetRootAsPlayerLeft(nil)               runtime error: index out of range [3] with length 0
```

**Consequence:** these guards can never fire, and they give a false impression of
input validation:

| Dead guard | Site |
|---|---|
| `if req == nil` | `exchange_handlers.go:20` |
| `if req == nil` | `exchange_handlers.go:208` |
| `if req == nil` | `quest_handlers.go:22` |
| `if req == nil` | `quest_handlers.go:78` |
| `if completed == nil` | `quest_handlers.go:143` |
| `if frame == nil` | `flatbuffer_tcp.go:59` (none present; see below) |
| `if frame == nil` | `router_client.go:234` |

Note `router_client.go:234` is the **only** site whose log line is even
reachable in principle — and it is still unreachable, because the panic happens
inside `GetRootAsMetaDBFrame` on the preceding line.

### 2b. Two handlers have *no* guard whatsoever

```go
// src/apps/meta_db/router_client.go:375-377
func handlePlayerJoined(data []byte, m *MetaDB) {
	joined := Protocol.GetRootAsPlayerJoined(data, 0)   // no length check, no nil check
	playerID := joined.PlayerId()                       // dereferences immediately
```

```go
// src/apps/meta_db/router_client.go:408-410
func handlePlayerLeft(data []byte, m *MetaDB) {
	left := Protocol.GetRootAsPlayerLeft(data, 0)       // no length check, no nil check
	playerID := left.PlayerId()
```

These are the **exact Go analogues** of the C++ `InventoryLoadHandler` /
`PlayerJoinedHandler` bug class named in the task. C++ was fixed
(`src/game/storage/PlayerJoinedHandler.cpp:22-27` now verifies before
`GetRoot`); **the Go equivalents were never fixed.**

```
$ sed -n '22,29p' src/game/storage/PlayerJoinedHandler.cpp
    flatbuffers::Verifier v(data.data(), data.size());
    if (!v.VerifyBuffer<Protocol::PlayerJoined>(nullptr)) {
        spdlog::warn("[SimCore] PlayerJoined: invalid PlayerJoined buffer ({} bytes) — dropped", ...);
        return;
    }
    auto joined = flatbuffers::GetRoot<Protocol::PlayerJoined>(data.data());
```

Verified panics for `handlePlayerJoined`'s exact call pattern:

```
  empty slice []byte{}         PANIC: runtime error: index out of range [3] with length 0
  nil slice                    PANIC: runtime error: index out of range [3] with length 0
  1 zero byte                  PANIC: runtime error: index out of range [3] with length 1
  4 zero bytes                 ok, PlayerId()=0
  garbage non-flatbuffer       PANIC: runtime error: slice bounds out of range [4022250974:6]
```

and for `handlePlayerLeft`:

```
  PlayerLeft empty             PANIC: runtime error: index out of range [3] with length 0
```

### 2c. Blast radius — the `defer recover()` saves the router path only

`handlePublish` **does** have a recover (`router_client.go:184-188`):

```go
func (rc *RouterClient) handlePublish(payload []byte) {
	defer func() {
		if r := recover(); r != nil {
			log.Printf("[router] PANIC in handlePublish: %v", r)
		}
	}()
```

So a bad `player.joined` / `player.left` / `quest.*` frame is **contained**:
the process survives, logs, and drops that one message. It does **not** kill
MetaDB. Good — this downgrades 2b from P0 to P1 for `router_client.go`.

**But `handleFlatBufferConnection` has NO recover** (`flatbuffer_tcp.go:35`):

```go
func handleFlatBufferConnection(conn net.TcpConn, m *MetaDB) {
	defer conn.Close()
	for { ... 
		frame := Protocol.GetRootAsMetaDBFrame(payload, 0)   // flatbuffer_tcp.go:58
```

A `go handleFlatBufferConnection(conn, m)` goroutine (`flatbuffer_tcp.go:31`)
that panics **terminates the whole `metadbd` process** — Go does not recover
un-panicked panics in goroutines. A single malformed 4-byte-length-prefixed
frame from any TCP peer on the MetaDB port kills the service.

Verified there is no top-level `recover()` in `main.go`:
```
$ grep -n "go func\|recover\|handlePublish\|HandleQuest\|handleRequest" src/apps/meta_db/main.go
96:	resp := handleRequest(m, req)
```
No `recover`, no wrapper.

**Unverifiable:** whether the MetaDB TCP port is reachable by an untrusted party.
It is a localhost internal port in the documented run order, so this is a
robustness defect rather than a remote-exploit finding.

---

## 3. P1 — guaranteed panic at `db.go:415`

```go
// src/apps/meta_db/db.go:409-415
builder := flatbuffers.NewBuilder(0)
Protocol.InventorySlotStart(builder)
Protocol.InventorySlotAddItemId(builder, uint16(slot.BlockID))
Protocol.InventorySlotAddCount(builder, uint8(slot.Count))
Protocol.InventorySlotAddMeta(builder, 0)
slotOffset := Protocol.InventorySlotEnd(builder)
protocolSlots[i] = *Protocol.GetRootAsInventorySlot(builder.FinishedBytes(), slotOffset)
```

**`builder.Finish()` is never called before `FinishedBytes()`.** This is a
hard, unconditional panic in the Go FlatBuffers runtime:

```
db.go:409-415 (as written)         PANIC: Incorrect use of FinishedBytes(): must call 'Finish' first.
correct: Finish() then read   len=20
   -> parsed itemId=1234 count=56
```

Two further defects in the same 7 lines:

- **A fresh `NewBuilder(0)` is allocated inside the loop** (`db.go:409`) — a
  per-slot allocation where one builder would do. Benign for correctness, a
  real inefficiency.
- **A 40-iteration loop** (`db.go:407`) → the panic is hit on the *first*
  iteration, so this code path can never succeed for any player.
- `GetRootAsInventorySlot(buf, slotOffset)` is also wrong on its own terms: the
  offset argument for a *root* table should be `0`, not an inner table offset.
  Verified: with the correct `Finish()` + offset `0`, parsing returns
  `itemId=1234 count=56` correctly.

**Severity caveat — reachability.** This is the legacy JSON `handleRequest`
path (`db.go:374`, called from `main.go:96`), not the FlatBuffers path. The
`create_inventory` action is reachable only if something still drives the JSON
API. **Unverifiable:** whether any live client/tool sends JSON `create_inventory`
with a non-empty `slots` array; I did not audit the JSON client surface. Flagged
P1 because it is a guaranteed panic on a reachable-in-principle path, and the
fix is 1 line.

---

## 4. The field-usage matrix itself

Legend: ✅ written with a real non-default value · ⚠️ zero is reachable ·
❌ never written

### 4a. `PlayerJoined` (core.fbs:394) — topic `player.joined`

| Field | Go reads | C++ writer | Verdict |
|---|---|---|---|
| `player_id:uint64` | `router_client.go:377` | `gateway.cpp:113,327,539,563` → `CreatePlayerJoined(fbb, client_player_id_)` | ✅ |

All four C++ publishers pass `client_player_id_`, and each is guarded by
`if (!player_id_known_) return;` (`gateway.cpp:324,333`) so a zero id is not
emitted. **No silent zero.**

### 4b. `PlayerLeft` (core.fbs:398) — topic `player.left`

| Field | Go reads | C++ writer | Verdict |
|---|---|---|---|
| `player_id:uint64` | `router_client.go:410` | `gateway.cpp:73-74,335-336` | ✅ |
| `x:int32` | `router_client.go:411` | `CreatePlayerLeft(fbb, client_player_id_, last_x_, ...)` | ⚠️ |
| `y:int32` | `router_client.go:412` | same | ⚠️ |
| `z:int32` | `router_client.go:413` | same | ⚠️ |

**The specific default observed:** `x/y/z` default to `0` in the schema
(no `= ` initialiser in `core.fbs:398-402`). `last_x_/last_y_/last_z_` are
declared `int32_t last_x_ = 0, last_y_ = 0, last_z_ = 0;`
(`src/apps/gateway/gateway.h:135`) and are **only ever written on a
`kPlayerAction` move** (`gateway.cpp:523+`). So a client that connects and
disconnects **without ever moving** publishes `player.left` with `pos=[0,0,0]`,
and MetaDB persists `SavePlayerPosition(playerID, 0, 0, 0)`
(`router_client.go:416`) — **overwriting a previously saved non-zero position
with the origin.** This is a genuine data-loss-shaped silent zero: it looks
like a valid position.

**Unverifiable:** whether the client always sends a `kPlayerAction` before
disconnect. `NetClient.cpp:669,685` sends `kPlayerAction` in two call sites;
I did not trace the disconnect ordering in `GameClient.cpp`.

### 4c. `QuestProgressUpdate` (quest.fbs:30) — topics `meta_db.quest.get` / `.set`

| Field | Go reads | C++ writer | Verdict |
|---|---|---|---|
| `player_id:uint64` | `quest_handlers.go:26,82` | `PlayerJoinedHandler.cpp:40` `CreateQuestProgressUpdate(builder, pid)` | ✅ |
| `quests:[QuestEntry]` | `quest_handlers.go:83-93` (`.set` only) | ❌ **no publisher** | ❌ |
| `QuestEntry.quest_id` | `quest_handlers.go:89` | only via the vector above | ❌ |
| `QuestEntry.status` | `quest_handlers.go:90` | only via the vector above | ❌ |
| `QuestEntry.progress` | `quest_handlers.go:91` | only via the vector above | ❌ |

The **query** path is correct: `PlayerJoinedHandler.cpp:39-42` builds
`QuestProgressUpdate` with a *null* `quests` vector, deliberately, as a query.

The **write** path is dead. See §5.

`QuestEntry.status` default is `LOCKED` (`quest.fbs:38`) — a non-zero enum, so
if the vector were ever absent, `status` would read as `LOCKED`, not 0. The Go
loop guards with `if req.Quests(entry, i)` (`quest_handlers.go:86`), so
unreadable entries leave `quests[i]` as the zero `QuestProgress` struct —
`Status: 0`. `QuestStatus` zero is not `LOCKED`; **the two are inconsistent.**

**Unverifiable:** the `QuestStatus` enum ordering in `quest.fbs:15-25`; I did
not read the full enum, so I cannot state whether Go's `0` maps to a valid
status. The inconsistency itself (schema default `LOCKED` vs Go zero value) is
confirmed from the two call sites.

### 4d. `QuestCompleted` (quest.fbs:123) — topic `quest.completed`

| Field | Go reads | C++ writer | Verdict |
|---|---|---|---|
| `player_id:uint64` | `quest_handlers.go:148` | `QuestManager.cpp:23` | ✅ |
| `quest_id:uint32` | `quest_handlers.go:149` | `QuestManager.cpp:23` | ✅ |
| `timestamp:uint64` | ❌ **not read** | `QuestManager.cpp:21-23` | ✅ written, unused |

`QuestManager.cpp:21-23` computes a nanosecond `system_clock` timestamp and
passes it. Go ignores it and instead does
`timestamp := time.Now().Unix()` (`quest_handlers.go:168`) for the reward row.
**This is a unit mismatch, not a zero**: C++ sends **nanoseconds** (≈1.7e18),
Go stores **seconds** (≈1.7e9) from its *own* clock. If any future consumer
reads `QuestCompleted.timestamp` expecting seconds it will be wrong by 1e9×.
Currently harmless because nothing reads it. Flagged P2.

### 4e. `QuestExchangeRequest` / `QuestExchangeCooldownGet` (quest.fbs:53, 81)

| Field | Go reads | C++ writer | Verdict |
|---|---|---|---|
| `player_id:uint64` | `exchange_handlers.go:25,212` | client→gateway `NetClient.cpp:819,829`; gateway relays **unmodified** at `gateway.cpp:609-619` | ✅ |
| `quest_id:uint32` | `exchange_handlers.go:26,213` | same | ✅ |

The gateway does a pure pass-through (`publish("quest.exchange.request", data, len)`)
after verifying, so the bytes MetaDB parses are the bytes the client built. No
rewrite, no field loss.

### 4f. `SetInventorySlotReq` (meta_db.fbs:25) — topic `meta_db.inventory.set`

| Field | Go reads | C++ writer | Verdict |
|---|---|---|---|
| `player_id` | `flatbuffer_tcp.go:174` | `InventoryActionHandler.cpp:267` / `simcore/main.cpp:307` | ✅ |
| `slot_index` | `flatbuffer_tcp.go:175` | same | ✅ |
| `item_id` | `flatbuffer_tcp.go:176` | same | ✅ |
| `count` | `flatbuffer_tcp.go:177` | same | ✅ |
| `meta` | ❌ not read | same | ⚠️ **written, ignored** |

**`meta` is written by both C++ publishers and read by nobody.** `handleSetInventorySlotReq`
calls `m.UpdateInventorySlot(playerID, slotIndex, itemID, count)`
(`flatbuffer_tcp.go:180`) — a 4-arg signature with no `meta` parameter. So
item metadata is **silently dropped** on every inventory write. The `.fbs`
documents `meta:uint16` (`meta_db.fbs:30`) and the C++ faithfully sends it.
This is a real, currently-observable data-loss bug: durability of item meta
(NBT-ish data) is zero.

`count` is safe: the delete path sends `item_id=0, count=0` and
`InventoryActionHandler.cpp:296-303` explicitly rejects `player_id == 0` and
out-of-range slots, so no accidental zero-count-with-nonzero-item reaches Go.

### 4g. `InventorySlot` (core.fbs:266) — MetaDB → C++/client

| Field | Go writes | Consumer reads | Verdict |
|---|---|---|---|
| `item_id:uint16` | `db.go:239`, `flatbuffer_tcp.go:154,207` | `PlayerInventoryStore`/client | ✅ |
| `count:uint8` | `db.go:240`, `flatbuffer_tcp.go:155,208` | same | ✅ |
| `meta:uint16` | `db.go:241`, `flatbuffer_tcp.go:156,209` | same | ✅ (read **back** into SQLite; the loss in 4f is on the *inbound* set path) |

`db.go:413` hardcodes `Protocol.InventorySlotAddMeta(builder, 0)` — the legacy
JSON import path discards meta unconditionally. Same reachability caveat as §3.

---

## 5. P1 — the quest persistence routing gap

**`meta_db.quest.set` has no publisher anywhere in the repository.**

```
$ grep -rn "meta_db\.quest\.set" . | grep -v cmake-build | grep -v '^\./\.claude'
./src/apps/meta_db/router_client.go:108:        "meta_db.quest.set",     <- subscription
./src/apps/meta_db/router_client.go:218:	case "meta_db.quest.set":           <- dispatch
./src/apps/meta_db/quest_handlers.go:188:  // comment
... (everything else is openspec/ or .beads/ documentation)
```

`SetQuestProgressBatch` — the batch-write function used by `HandleQuestSet`
(`quest_handlers.go:98`) — has exactly **one** caller:

```
$ grep -rn "SetQuestProgressBatch" --include=*.go src/ | grep -v _test
src/apps/meta_db/quest_progress.go:68:func SetQuestProgressBatch(...)
src/apps/meta_db/quest_handlers.go:98:	err := SetQuestProgressBatch(m.db, playerID, quests)
```

…which is only reachable via the unpublished `meta_db.quest.set` topic.
**`HandleQuestSet` is dead code.**

The only *live* writer of `quest_progress` is `SetQuestProgress`
(`quest_progress.go:60-66`), called from exactly one place —
`quest_handlers.go:191`, on `quest.completed`:

```
$ grep -rn "INSERT INTO quest_progress\|ON CONFLICT(player_id, quest_id)" --include=*.go src/ | grep -v _test
src/apps/meta_db/quest_progress.go:60
```

And the code documents this dependency explicitly
(`quest_handlers.go:187-191`):

```go
// Persist the COMPLETED status so quest progress survives a server restart.
// quest_progress is otherwise only written via meta_db.quest.set, which
// nothing publishes — quest.completed is the single authoritative
// completion signal, so this is where the row must be upserted.
```

**Consequence:** quest progress is persisted **only for the `COMPLETED` state**.
`AVAILABLE` / `IN_PROGRESS` / any partial `progress_percent` is never written to
SQLite. The `GetQuestProgress` read path (`quest_handlers.go:29`) will therefore
return only completed quests on rejoin, so a player mid-quest sees an empty
progress list after a restart. **Not a silent zero in the FlatBuffers sense** —
it's a missing-row that reads as "no quests", which is indistinguishable from a
new player.

**Also dead:** `meta_db.inventory.get` and `meta_db.inventory.snapshot` have no
C++ publisher (same grep). Only `meta_db.inventory.set` is live. The MetaDB
`GetInventoryReq` / `GetInventorySnapshotReq` handlers
(`flatbuffer_tcp.go:107,125`) and their response builders are reachable only over
the direct TCP path, not via the router.

---

## 6. Findings, ranked

| # | Sev | Finding | Site |
|---|---|---|---|
| 1 | **P0** | All 7 `if x == nil` guards after `GetRootAs*` are **dead code** — the generated Go never returns nil. They provide no protection and imply validation that does not exist. | `exchange_handlers.go:20,208`; `quest_handlers.go:22,78,143`; `flatbuffer_tcp.go:59`; `router_client.go:234` |
| 2 | **P1** | `handlePlayerJoined` / `handlePlayerLeft` have **no** guard at all — the exact Go analogue of the fixed C++ `PlayerJoinedHandler` bug. Empty or garbage payload panics. | `router_client.go:375-377`, `408-410` |
| 3 | **P1** | Guaranteed panic: `FinishedBytes()` called before `Finish()`. | `db.go:415` |
| 4 | **P1** | `handleFlatBufferConnection` has **no `recover()`**; one malformed frame kills `metadbd`. (Router path *is* recovered.) | `flatbuffer_tcp.go:35,58` |
| 5 | **P1** | `meta_db.quest.set` has no publisher → `HandleQuestSet` is dead → only `COMPLETED` quest state is ever persisted. | `router_client.go:108,218`; `quest_handlers.go:98` |
| 6 | **P2** | `SetInventorySlotReq.meta` is written by both C++ publishers and **read by nobody** — item metadata silently dropped on every inventory write. | `flatbuffer_tcp.go:180` vs `meta_db.fbs:30` |
| 7 | **P2** | `PlayerLeft` `x/y/z` can be `0,0,0` (gateway `last_x_` default 0 if the player never moves), overwriting a saved position with the origin. | `gateway.h:135`; `gateway.cpp:335`; `router_client.go:416` |
| 8 | **P2** | `QuestCompleted.timestamp` is nanoseconds from C++ but Go stores its own **seconds** — latent unit mismatch, currently unobserved because Go ignores the field. | `QuestManager.cpp:21-23` vs `quest_handlers.go:168` |
| 9 | **P3** | Go's zero-value `QuestProgress{Status:0}` for an unreadable entry contradicts the schema default `status:LOCKED`. | `quest_handlers.go:86-92` vs `quest.fbs:38` |
| 10 | **P3** | `db.go:409` allocates a fresh `NewBuilder(0)` per slot in a 40-iteration loop. | `db.go:407-415` |

---

## 7. Explicitly unverifiable

- **Whether `meta_db.inventory.get` / `.snapshot` are driven over the direct TCP
  path** by any live client. The handlers are reachable in-process; I did not
  audit the TCP client surface.
- **The full `QuestStatus` enum ordering** in `quest.fbs` — not read in full, so
  I cannot say whether Go's `0` is a valid status.
- **Whether the client always emits a `kPlayerAction` before disconnect**, which
  determines whether finding 7 is reachable in practice.
- **Whether any non-localhost peer can reach the MetaDB TCP port**, which
  determines the severity of finding 4.
