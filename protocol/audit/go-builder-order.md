# Audit gp-3dw6 — FlatBuffers builder call order + missing verification in the Go sidecars

**Scope:** read-only audit of `src/apps/meta_db` and `src/apps/message_router`.
No `.fbs`, `.go`, or `.cpp` file was modified.
**Date:** 2026-09-26

---

## TL;DR

- **Scope is MetaDB only.** `src/apps/message_router` contains **zero** FlatBuffers
  code — it is a pure byte router. Nothing in it to audit.
- **13 builder sequences** were audited. **12 are correctly ordered.**
  The `*Direct`-style `PrependUOffsetT` pattern that the task flagged as a risk
  is, in Go, **correct** — I verified it empirically rather than assuming.
- **1 builder is genuinely broken**: `db.go:409-415` calls `FinishedBytes()`
  before `Finish()`, which is an **unconditional panic**.
- **The verifier gap is real and worse than described.** All 7 `== nil` guards
  after `GetRootAs*` in the Go code are **dead code** — the generated accessor
  never returns nil. Two handlers have no guard at all. The C++ `Verifier`
  hardening (`InventoryLoadHandler`, `PlayerJoinedHandler`) has **no Go counterpart**.

---

## 1. Scope confirmation — `message_router` is not in scope

```
$ grep -rn "flatbuffers\|Protocol\." --include=*.go src/apps/message_router
(no output)
```

The router only manipulates raw bytes:
`src/apps/message_router/router.go:582-593` writes
`[4B len][1B type][2B topicLen][topic][opaque payload]`, and
`readFrame` (`:554-561`) reads the length prefix. It never parses, copies into,
or re-serialises a FlatBuffer, so it **cannot corrupt a field** — and equally
offers no validation. It is a pass-through, correctly.

Consequently the "empty buffer where `data()` is nil" concern is **Go-specific**:
in Go there is no `data()`/`nullptr`; the equivalent hazard is a **short slice**,
because the generated `GetUOffsetT(buf[offset:])` indexes before any bounds
concept exists. Covered in §4.

---

## 2. Builder-order audit — all 13 sequences

The FlatBuffers back-to-front rule: nested objects must be **created before** the
vector/table that references them, and `Finish()` **last**.

| # | Site | Shape | Nested-before-parent? | `Finish()` last? | Verdict |
|---|---|---|---|---|---|
| 1 | `db.go:235-257` `PublishInventoryTo` | N×`InventorySlot` → vector → `InventoryUpdate` | ✅ slots built in loop, vector `:245-249`, then table `:251-254` | ✅ `:255` | ✅ correct |
| 2 | **`db.go:409-415`** | `InventorySlot` per loop | n/a | ❌ **`Finish()` never called** | ❌ **BROKEN — §3** |
| 3 | `flatbuffer_tcp.go:149-170` | N×`InventorySlot` → vector → `GetInventoryResp` → `buildReplyFrame` | ✅ `:152-164`, table `:166-168` | ✅ in `buildReplyFrame:242` | ✅ correct |
| 4 | `flatbuffer_tcp.go:186-192` | `CreateByteVector` → `GetInventoryResp` | ✅ vector `:187` **before** table `:188` | ✅ `:242` | ✅ correct |
| 5 | `flatbuffer_tcp.go:202-227` | N×`InventorySlot` → **two** vectors → `GetInventorySnapshotResp` | ✅ `:205-217` (main), `:219-220` (empty hotbar), then table `:222-225` | ✅ `:242` | ✅ correct |
| 6 | `flatbuffer_tcp.go:230-243` `buildReplyFrame` | resp → `MetaDBReply` → `MetaDBFrame` | ✅ `:231-235` then `:237-240` | ✅ `:242` | ✅ correct — correct two-level union wrapping |
| 7 | `flatbuffer_tcp.go:247-254` `buildErrorResp` | `CreateString` → `ErrorResp` → reply frame | ✅ string `:248` before table `:250` | ✅ `:242` | ✅ correct |
| 8 | `quest_handlers.go:35-56` `HandleQuestGet` | N×`QuestEntry` → vector → `QuestProgressUpdate` | ✅ `:38-50`, table `:52-55` | ✅ `:56` | ✅ correct |
| 9 | `quest_handlers.go:105-125` `HandleQuestSet` | same shape | ✅ `:107-119`, table `:121-124` | ✅ `:125` | ✅ correct (but **dead code** — `field-usage-matrix.md` §5) |
| 10 | `quest_handlers.go:198-205` `QuestCompletedNotification` | flat, no nested | n/a | ✅ `:205` | ✅ correct |
| 11 | `exchange_handlers.go:30-42` `reply` closure | **conditional** `CreateString` → `QuestExchangeResponse` | ✅ string `:33` before table `:35` | ✅ `:42` | ✅ correct, **and** correctly guards `errOff` to `0` when `errMsg == ""` (`:32-34`) so the field is omitted rather than written as a bogus offset |
| 12 | `exchange_handlers.go:229-235` | flat | n/a | ✅ `:235` | ✅ correct |
| 13 | `router_client.go:396-403` | flat `PlayerLeft` | n/a | ✅ `:403` | ✅ correct |

**No ordering violation found in any of the 12 non-broken sequences.**

---

## 3. The one real builder bug — `db.go:409-415`

```go
// src/apps/meta_db/db.go:409-415
for i, slot := range slots {
    builder := flatbuffers.NewBuilder(0)
    Protocol.InventorySlotStart(builder)
    Protocol.InventorySlotAddItemId(builder, uint16(slot.BlockID))
    Protocol.InventorySlotAddCount(builder, uint8(slot.Count))
    Protocol.InventorySlotAddMeta(builder, 0)
    slotOffset := Protocol.InventorySlotEnd(builder)
    protocolSlots[i] = *Protocol.GetRootAsInventorySlot(builder.FinishedBytes(), slotOffset)
}
```

`builder.Finish()` is **never called**. The Go FlatBuffers runtime enforces this:

```
db.go:409-415 (as written)         PANIC: Incorrect use of FinishedBytes(): must call 'Finish' first.
correct: Finish() then read   len=20
   -> parsed itemId=1234 count=56
```

Three distinct defects in these 7 lines:

| # | Defect |
|---|---|
| a | **`Finish()` omitted** → unconditional panic on the first loop iteration |
| b | **`GetRootAsX(buf, offset)` misused** — the second arg is the *root offset*, which is `0`. `slotOffset` is an inner table offset. Even with `Finish()` added, this reads the wrong location. |
| c | **`NewBuilder(0)` re-allocated inside the loop** (`:409`) — a fresh builder per slot, inside a 40-iteration loop (`:407`). |

Correct form, verified:

```go
builder := flatbuffers.NewBuilder(0)
Protocol.InventorySlotStart(builder)
...
slotOffset := Protocol.InventorySlotEnd(builder)
builder.Finish(slotOffset)
slot := Protocol.GetRootAsInventorySlot(builder.FinishedBytes(), 0)  // root offset 0
```

**Reachability caveat (unverifiable):** this is the legacy JSON `handleRequest`
path (`db.go:374`, invoked from `main.go:96`), not the FlatBuffers router path.
It fires only if a caller sends JSON `{"action":"create_inventory", …}` with a
non-empty `slots` array. I did not audit the JSON client surface, so I cannot
confirm a live trigger. It is a guaranteed panic on a code path that is
syntactically reachable and has no guard.

---

## 4. The verifier gap — the Go analogue of the C++ bug is **unfixed**

### 4a. The root cause: `GetRootAsX` never returns nil

Every generated accessor is nil-free by construction
(`src/protocol/generated/go/Protocol/PlayerJoined.go:13-19`):

```go
func GetRootAsPlayerJoined(buf []byte, offset flatbuffers.UOffsetT) *PlayerJoined {
	n := flatbuffers.GetUOffsetT(buf[offset:])   // panics on short buf
	x := &PlayerJoined{}
	x.Init(buf, n+offset)                       // no error path
	return x                                    // never nil
}
```

Verified against a `nil` slice — **every** accessor panics, so **no** guard can fire:

```
  GetRootAsQuestExchangeRequest(nil)     runtime error: index out of range [3] with length 0
  GetRootAsQuestExchangeCooldownGet(nil) runtime error: index out of range [3] with length 0
  GetRootAsQuestProgressUpdate(nil)      runtime error: index out of range [3] with length 0
  GetRootAsQuestCompleted(nil)           runtime error: index out of range [3] with length 0
  GetRootAsMetaDBFrame(nil)              runtime error: index out of range [3] with length 0
  GetRootAsPlayerJoined(nil)             runtime error: index out of range [3] with length 0
  GetRootAsPlayerLeft(nil)               runtime error: index out of range [3] with length 0
```

### 4b. All 7 nil-guards are dead code

| Site | Written guard | Reality |
|---|---|---|
| `exchange_handlers.go:20` | `if req == nil` | unreachable |
| `exchange_handlers.go:208` | `if req == nil` | unreachable |
| `quest_handlers.go:22` | `if req == nil` | unreachable |
| `quest_handlers.go:78` | `if req == nil` | unreachable |
| `quest_handlers.go:143` | `if completed == nil` | unreachable |
| `flatbuffer_tcp.go:59` | *(absent)* | `GetRootAsMetaDBFrame` at `:58` panics first |
| `router_client.go:234` | `if frame == nil` | unreachable (panic on `:233`) |

These are worse than no check: the log line `log.Printf("… failed to parse …")`
gives the false impression that malformed input is being handled.

### 4c. Two handlers have **no** guard whatsoever

```go
// src/apps/meta_db/router_client.go:375-377
func handlePlayerJoined(data []byte, m *MetaDB) {
	joined := Protocol.GetRootAsPlayerJoined(data, 0)   // no length check, no nil check
	playerID := joined.PlayerId()                       // immediate dereference
```

```go
// src/apps/meta_db/router_client.go:408-410
func handlePlayerLeft(data []byte, m *MetaDB) {
	left := Protocol.GetRootAsPlayerLeft(data, 0)       // no length check, no nil check
	playerID := left.PlayerId()
```

These are the **direct Go counterparts** of the two C++ sites named in the task.
C++ was fixed — `PlayerJoinedHandler.cpp:22-27` now runs a `Verifier` before
`GetRoot`, with a comment naming the exact gp-ajvg incident:

```cpp
flatbuffers::Verifier v(data.data(), data.size());
if (!v.VerifyBuffer<Protocol::PlayerJoined>(nullptr)) {
    spdlog::warn("[SimCore] PlayerJoined: invalid PlayerJoined buffer ({} bytes) — dropped", data.size());
    return;
}
auto joined = flatbuffers::GetRoot<Protocol::PlayerJoined>(data.data());
```

**The Go equivalents were never hardened.** Verified panics for the exact
`handlePlayerJoined` call pattern:

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

Note the **4-zero-bytes case returns `PlayerId() == 0` without panicking** — that
is the genuine silent path: a 4-byte payload of zeros parses as a valid
`PlayerJoined` with `player_id = 0`, and MetaDB would then read/overwrite
**player 0's** inventory row.

### 4d. Blast radius differs per path

| Path | Has `recover()`? | Result |
|---|---|---|
| `handlePublish` (`router_client.go:184-188`) | ✅ **yes** | panic contained; message dropped, process survives. Covers `player.joined`, `player.left`, `quest.*`. |
| `handleFlatBufferConnection` (`flatbuffer_tcp.go:35`) | ❌ **no** | un-panicked goroutine panic **kills `metadbd`**. |

`flatbuffer_tcp.go:31` spawns `go handleFlatBufferConnection(conn, m)`. Go does
not recover goroutine panics, and `main.go` has no top-level `recover`:

```
$ grep -n "go func\|recover\|handlePublish\|HandleQuest\|handleRequest" src/apps/meta_db/main.go
96:	resp := handleRequest(m, req)
```

So one malformed length-prefixed frame from any TCP peer on the MetaDB port is a
full-process DoS. **Unverifiable:** whether that port is reachable by an untrusted
party — it is localhost-internal in the documented run order, so this is a
robustness defect, not a remote exploit.

---

## 5. The `PrependUOffsetT` question — explicitly cleared

The task flagged the C++ `PrependUOffsetTRelative` vs `PrependUOffsetT` bug class.
In **Go**, the five reverse-loop vector sites (`db.go:245-249`,
`flatbuffer_tcp.go:160-164`, `:213-217`, `quest_handlers.go:46-50`, `:115-119`)
use:

```go
Protocol.XStartVector(builder, n)
for i := n - 1; i >= 0; i-- {
    builder.PrependUOffsetT(offs[i])
}
vec := builder.EndVector(n)
```

This is the **canonical FlatBuffers-Go idiom** — and because
`Protocol.XStartVector` + `PrependUOffsetT` + `EndVector` bypasses the generated
`builder.CreateVector`, the reverse order is what makes the resulting vector
read back in the original order. Verified end-to-end for 0/1/2/3/5 entries:

```
n=0   len=40   playerId=7 questsLen=0 ->  readOK=0/0
n=1   len=56   playerId=7 questsLen=1 -> [100]  readOK=1/1
n=2   len=88   playerId=7 questsLen=2 -> [100] [101]  readOK=2/2
n=3   len=104  playerId=7 questsLen=3 -> [100] [101] [102]  readOK=3/3
n=5   len=136  playerId=7 questsLen=5 -> [100] [101] [102] [103] [104]  readOK=5/5
```

**Element order is preserved and every entry round-trips.** No bug here.

One nuance worth recording: sites 1, 3, 8, 9 (and 5 for the hotbar vector) all
use this idiom, so it is consistently applied — the codebase does not mix
`PrependUOffsetT` with `CreateVector` in a way that could double-reverse.
`flatbuffer_tcp.go:187` uses `builder.CreateByteVector(nil)` for an empty
inventory — different API, no ordering hazard.

---

## 6. Findings, ranked

| # | Sev | Finding | Site |
|---|---|---|---|
| 1 | **P0** | All 7 `== nil` guards after `GetRootAs*` are **dead code** — the generated accessor never returns nil. They assert validation that does not exist. | `exchange_handlers.go:20,208`; `quest_handlers.go:22,78,143`; `flatbuffer_tcp.go:59`; `router_client.go:234` |
| 2 | **P1** | `handlePlayerJoined` / `handlePlayerLeft` have **no** guard at all — the unfixed Go analogue of the C++ `PlayerJoinedHandler` fix. Empty/garbage payload panics. | `router_client.go:375-377`, `408-410` |
| 3 | **P1** | Guaranteed panic: `FinishedBytes()` before `Finish()`. Plus a wrong root offset and a per-iteration builder allocation. | `db.go:409-415` |
| 4 | **P1** | `handleFlatBufferConnection` has **no `recover()`** — one malformed frame kills `metadbd`. | `flatbuffer_tcp.go:31,35,58` |
| 5 | **P2** | A 4-byte all-zeros payload parses cleanly as `PlayerJoined{player_id:0}` — silent wrong-player read/write instead of a panic. | `router_client.go:377` |
| 6 | **P3** | `HandleQuestSet`'s builder (`quest_handlers.go:105-125`) is correctly ordered but the handler is **unreachable** (no publisher for `meta_db.quest.set`). | `router_client.go:108,218` |

---

## 7. Explicitly unverifiable

- **Whether the JSON `create_inventory` path (`db.go:374`) is reachable** by any
  live client. Determines whether finding 3 is a live crash or a latent one.
- **Whether the MetaDB TCP port is reachable by a non-localhost peer** — sets the
  severity of finding 4.
- **Whether the 4-zero-bytes `PlayerJoined` case (finding 5) can be induced** by
  any real publisher. The MessageRouter framing would have to deliver exactly
  4 zero bytes; I did not audit the router's payload-length validation
  (`router.go:582-593` does not appear to inspect payload content).
