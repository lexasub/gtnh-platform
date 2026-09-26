# Audit gp-7d1 — Go sidecars with no error wrapping at process boundaries

**Date:** 2026-09-26 · **Scope:** `src/apps/meta_db/*.go`, `src/apps/message_router/*.go` (non-test) · **Mode:** read-only
**Lines audited:** 4577 total, 2573 production (meta_db 2534, message_router 793).

## Method

```bash
cd /home/su/src/local/gtnh-platform
wc -l src/apps/meta_db/*.go src/apps/message_router/*.go
grep -rn "recover()"  src/apps/meta_db/*.go src/apps/message_router/*.go
grep -rn "conn.Write\|\.Write("  src/apps/meta_db/*.go src/apps/message_router/*.go
grep -rn "rows.Next()\|rows.Err()" src/apps/meta_db/*.go
grep -rn "return err$" src/apps/meta_db/*.go src/apps/message_router/*.go
```

**Live verification:** the two highest-severity findings were confirmed by running faithful extractions
of the code under test (`/home/su/.hermes/cache/scratch/go_probe/main.go`, `go run main.go`). Results
are quoted inline as **[probe]**.

## The reference pattern — confirmed, and it is correct

The brief asked me to confirm that `readRouterFrame` now returns independently-owned payloads after
the queued-payload aliasing fix.

`src/apps/meta_db/router_client.go:257-283` — **confirmed correct:**

```go
func readRouterFrame(conn net.Conn, buf []byte) (msgType, []byte, error) {
	header := buf[:5]
	if _, err := io.ReadFull(conn, header); err != nil { return 0, nil, err }
	...
	// Return an independently owned slice even when buf is reusable. The
	// caller queues this payload for asynchronous handling, so aliasing buf
	// would let the next frame overwrite bytes before they are processed.
	payload = make([]byte, totalLen)
	if _, err := io.ReadFull(conn, payload); err != nil { return 0, nil, err }
```

`payload` is always `make`d, never a subslice of `buf`. The caller queues it on `frameCh` (buffered 64)
at `router_client.go:140` and processes it later at `:159`, so ownership transfer is real. **This is the
pattern the rest of the codebase should follow**, and it is the reason Finding 1 below is a live bug
rather than a theoretical one.

---

## Finding 1 — `message_router.readFrame` still has the exact bug that was just fixed in meta_db  · SEVERITY: critical

`src/apps/message_router/router.go:571-575`, caller `src/apps/message_router/main.go:85-123`

```go
	var payload []byte
	if totalLen <= cap(buf)-frameHeaderSize {
		payload = buf[frameHeaderSize : frameHeaderSize+totalLen]   // <-- line 572: ALIASES the caller's buf
	} else {
		payload = make([]byte, totalLen)
	}
```

`buf` is allocated **once per connection** at `main.go:85` (`buf := make([]byte, 64*1024)`) and reused
for every frame in the read loop at `main.go:89`. So for any payload that fits in 64 KB — which is
essentially every publish, since the chunk payload cap is far below that — the returned `payload` is a
window into the buffer that the *next* `readFrame` call will overwrite.

Why it is not caught today: within `handleConn` the frame is consumed **synchronously** in the same
iteration (`:99-163`) and `buf` is not reused until the loop comes back around. So the aliasing is
currently latent rather than active.

Why it is critical anyway:

1. **The caller shape is one refactor away from the bug that was just fixed.** `meta_db` had exactly the
   safe-looking structure and it broke the moment a payload was queued for async handling. Here
   `Router.Publish` is a synchronous fan-out, but the `healthSnapshot` path already hands payloads to a
   **goroutine** (`router.go:317-320`: `go func() { time.Sleep(...); r.finishHealthProbe(nonce) }()`),
   and `MsgPublish` → `r.Publish` copies into pooled frames, so those two are currently safe by
   accident. Any future async use of the `payload` slice re-opens a data-corruption bug that has
   already been paid for once in this codebase.
2. It is the exact defect the last fix removed, in the sibling process, unfixed.

**Recommendation (not applied):** apply the `readRouterFrame` shape — always `make`, never alias.

## Finding 2 — a truncated `Register` frame produces 65534 log lines and a partial topic list  · SEVERITY: high

`src/apps/message_router/main.go:135-148`

```go
			nTopics := int(binary.BigEndian.Uint16(payload[offset : offset+2]))
			offset += 2

			topics := make([]string, 0, nTopics)
			for i := 0; i < nTopics; i++ {
				topic, newOffset, err := readString(payload, offset)
				if err != nil {
					log.Printf("[conn] bad register frame (topic %d): %s err=%v", i, conn.RemoteAddr(), err)
					continue                 // <-- line 143: offset NOT advanced
				}
				topics = append(topics, topic)
				offset = newOffset
			}
			r.RegisterService(name, topics, cl)
```

On a decode error the loop `continue`s **without advancing `offset`**, so every subsequent iteration
re-reads the same bad bytes and fails identically. `nTopics` is attacker- or bug-controlled up to 65535.

**[probe]:**
```
=== TEST 1: register frame with a truncated topic list ===
declared nTopics=65535 -> 65535 iterations, 65534 log lines, 1 topics accepted
```

So a single malformed frame yields ~65 k log lines (a log-volume DoS against the shared
`log` output), and the service is then registered with a **truncated topic list** — it silently
subscribes to a subset of the topics it asked for, with no indication in the `register:` line that
anything was dropped. The `continue` in the `MsgSubscribe`/`MsgUnsubscribe`/`MsgPublish` cases
(`:104, :112, :120`) is correct because those loops advance `offset`; only this inner loop is wrong.

**Recommendation:** `break` on error (and log the truncation once), or advance `offset` defensively.

## Finding 3 — unverified `GetRootAs*` on the router path, with no `recover()` in `handleConn`  · SEVERITY: high

`src/apps/message_router/main.go:31` (`go handleConn(router, conn)`), `:89-99`

`main.go` has **no `recover()` anywhere** — the only one in either sidecar is
`router_client.go:185`, inside `handlePublish`. Every `GetRootAs*` in the Go sidecars therefore trusts
the buffer completely.

The C++ side was already hardened for this exact class (`IoUringChunkStoreService.cpp:71-78` runs a
`flatbuffers::Verifier` before `GetRoot`; the two handlers that crashed SimCore were fixed for it).
**Go has no equivalent.** The generated Go accessor is unchecked:

`src/protocol/generated/go/Protocol/PlayerJoined.go:13-17`
```go
func GetRootAsPlayerJoined(buf []byte, offset flatbuffers.UOffsetT) *PlayerJoined {
	n := flatbuffers.GetUOffsetT(buf[offset:])   // panics on short buf
	...
```

**[probe]:**
```
=== TEST 2: GetRootAs* on an empty/short buffer ===
panicked=true msg=runtime error: index out of range [3] with length 0
```

A Go panic on a **non-`main` goroutine** that has no `recover()` terminates the entire process — it
cannot be caught by the connection handler. On the meta_db side the router path is *partially*
protected: `handleRouterFrame` (line 159) runs on the `connectAndServe` goroutine, not a fresh one, so
the `recover()` at `router_client.go:185` does **not** cover `handleRouterFrame` itself — it only covers
`handlePublish`. The `msgHealthRequest` branch (line 171) and the `default:` log (line 177) are
unprotected.

`meta_db` also has an unprotected path: `startFlatBufferListener` (line 31 `go handleFlatBufferConnection`)
calls `Protocol.GetRootAsMetaDBFrame(payload, 0)` at `flatbuffer_tcp.go:58` with **no length check and
no `recover()`**. Any malformed client on the `:5006` port kills `metadbd`.

**Recommendation:** run every `GetRootAs*` behind a length check, and add `defer recover()` to
`handleConn` and `handleFlatBufferConnection`.

## Finding 4 — `writePublish` / `writeRegister` / `writeSubscribe` are `void`; a failed publish is one log line  · SEVERITY: high

`src/apps/meta_db/router_client.go:300-352`

```go
func writePublish(conn net.Conn, topic string, data []byte) {
	...
	if err := writeFrame(conn, msgPublish, payload); err != nil {
		log.Printf("[router] publish error: %v", err)   // <-- swallowed
	}
}
```

The write error is logged and the function returns `void`, so no caller can react. Every save
acknowledgement MetaDB sends goes through here:

- `router_client.go:247` — `writePublish(rc.conn, replyTopic, respData)`, the reply to an RPC
- `db.go:257` — `m.rc.PublishRaw(topic, builder.FinishedBytes())`, the inventory publish on
  `player.joined`
- `router_client.go:405` — `m.rc.PublishRaw("player.position.load", ...)`
- `quest_handlers.go:61,128,208` — quest progress/completion notifications

A failed publish means **the client believes a save succeeded and the server has no record of it**,
with one log line and no retry and no metric. `PublishRaw` (line 355-363) adds a second swallow: a
`nil` `conn` (before the first connect completes) returns silently with no log at all.

Only one publish path does propagate: `writeHeartbeat` (line 366-369) returns `error` and its failure
reconnects the client (`router_client.go:150-154`). That is the correct shape — make the others match.

`writeFrame` (line 294) also **discards the byte count**: `_, err := conn.Write(frame)`. A short write
is reported as success. In practice `net.TCPConn.Write` retries internally, so this is low severity,
but it is the same family.

## Finding 5 — the connection mutex is not held across a write, and the conn is read unlocked  · SEVERITY: medium

`src/apps/meta_db/router_client.go:171`

```go
	case msgHealthRequest:
		if err := writeFrame(rc.conn, msgHealthResponse, payload); err != nil {
```

`rc.conn` is read **without** `rc.mu`, while `Stop()` (line 61-65) writes it under `rc.mu`. `Stop()`
closes the conn concurrently, so this can write to a closing connection. `PublishRaw` (line 356-358)
does take the lock — so the correct pattern already exists 200 lines above. Minor, but it is a real
data race under `-race` and the fix is one lock acquisition.

## Finding 6 — the drop counter is maintained but never read  · SEVERITY: medium

`src/apps/message_router/router.go:81` declares `dropped atomic.Uint64`; incremented at
`:431, 441, 455, 460, 466`. **`grep -rn "\.dropped"` shows no read anywhere.** The only visibility is the
aggregate `log.Printf` at `router.go:474-476`, which fires only when `dropped > 0` **for that publish
call** and is never aggregated or exported.

So: when a slow client's `sendCh` (capacity 4096, `router.go:40`) fills, `Publish` silently evicts that
client's **oldest** queued frame (lines 447-449) and increments a counter nobody reads. For
`world.*` topics that is inventory or block-entity updates dropped with no metric. The
`SIGUSR1` metrics dump (`main.go:57-63`) shows client and topic counts but not drops.

This is the Go-side mirror of the C++ "write reported as successful" class, and it is the one most
likely to be diagnosed late, because nothing anywhere says a message was lost.

---

## Error-wrapping (`%w`) audit

Go convention is `%w` so `errors.Is`/`errors.As` can walk the chain. This codebase wraps
**only in `NewMetaDB`**:

| Site | Wrapped? | Note |
|---|---|---|
| `db.go:101` `failed to open database: %w` | yes | |
| `db.go:104` `failed to ping database: %w` | yes | |
| `db.go:109` `failed to initialize schema: %w` | yes | |
| `db.go:319,323` `DeleteEntityState` | yes | the only place that adds context to a SQL error |
| **`db.go:116, 126, 131, 134, 139, 145, 205, 210, 215, 273, 288, 310`** | **no** | bare `return err` from `m.db.Exec`/`tx.Exec` |
| `definitions.go:88, 95, 100, 104` | no | bare `return err` |
| `quest_progress.go:64` | no | bare `return err` |
| `router_client.go:92, 153, 295` | n/a | `net` errors, already contextual ("connect and serve") |
| `flatbuffer_tcp.go:144, 182, 199` | n/a | string-concatenated into an error **response**: `"GetInventory failed: " + err.Error()` — the wrap is for the client, not for `errors.Is` |

The bare `return err` sites are the ones that matter for diagnosis. Concretely, when
`UpdateInventorySlot` fails (`db.go:205`), the log line at `flatbuffer_tcp.go:178` reports the handler
and player, and the client gets `"UpdateInventorySlot failed: <sqlite text>"` — but nothing in the log
says **which SQL statement** or whether it was a constraint violation, a locked DB, or disk I/O. With
`fmt.Errorf("update inventory slot (player=%d slot=%d): %w", ...)` the same failure is diagnosable
from the log alone. There are **12** such sites in `db.go` alone, all in the persistence layer.

**Credit where due:** `db.go:334-338` is the one place that gets severity badly wrong —
`GetPlayerCount` turns a SQL error into `0` with no log, so "database is broken" and "no players have
ever joined" are indistinguishable.

`rows.Err()` coverage is good everywhere **except** `db.go:183-194`:
`quest_progress.go:51`, `reward_handlers.go:71,102,133,194`, and `exchange_handlers.go:124` all check
it; `GetInventory` — the function called on **every** `player.joined` — does not. A mid-stream DB error
truncates the result set and a short inventory gets published to the client as the player's real one.

---

## Ranked

| # | Site | Consequence |
|---|---|---|
| 1 | `router.go:571-575` | same aliasing defect just fixed in meta_db; live in `message_router` |
| 2 | `main.go:140-147` | one bad frame → 65 k log lines + silent topic truncation |
| 3 | `flatbuffer_tcp.go:58`, `main.go:89-99` | unverified `GetRootAs*`, no `recover()` → **process dies** |
| 4 | `router_client.go:340-352` | failed publish = one log line; client thinks the save landed |
| 5 | `router.go:81, 431-466` | drop counter never read; message loss invisible |
| 6 | `db.go:205` et al. (12 sites) | bare `return err`, no SQL context |
| 7 | `db.go:183-194` | missing `rows.Err()` on the `player.joined` path |
| 8 | `router_client.go:171` | `rc.conn` read without the mutex |
| 9 | `db.go:334-338` | SQL error → `0`, unlogged |

## Correctly handled — stated so the list is not padded

- **`readRouterFrame` (`router_client.go:257-283`)** — the reference pattern. Always `make`s, rejects
  `payloadLen < 1`, checks every `io.ReadFull`. Correct.
- `message_router/main.go:91` — `if err != io.EOF && !isClosedConn(err)` correctly avoids logging a
  normal disconnect as an error. Good hygiene.
- `isClosedConn` (`:167-175`) — narrow, correct, no string-matching on unrelated errors.
- `writeHeartbeat` returning `error` and driving the reconnect (`:366-369`, `:150-154`) — the model
  the other write helpers should copy.
- `writeFrame` (`:286-296`), `writeRegister` (`:300-324`), `writeSubscribe` (`:328-336`) — buffer
  construction and length-prefix encoding are correct; only the discarded error is at fault.
- `db.go:114-150` `CreateInventory` — `defer tx.Rollback()` plus explicit `tx.Commit()` return. Correct.
- `db.go:313-329` `DeleteEntityState` — checks `RowsAffected()` and reports a not-found as an error.
  The only DB method with that rigor.
- `handlePublish`'s bounds checks (`:189-195`) and its `recover()` (`:184-188`) — correct, and the
  model for the missing guards in Finding 3.

## Commands run

```bash
wc -l src/apps/meta_db/*.go src/apps/message_router/*.go
grep -rn "recover()" src/apps/meta_db/*.go src/apps/message_router/*.go
grep -rn "conn.Write\|\.Write(" src/apps/meta_db/*.go src/apps/message_router/*.go
grep -rn "rows.Next()\|rows.Err()" src/apps/meta_db/*.go
grep -rn "return err$" src/apps/meta_db/*.go src/apps/message_router/*.go
grep -rn "\.dropped" src/apps/message_router/*.go
awk 'NR>=554 && NR<=580' src/apps/message_router/router.go
awk 'NR>=125 && NR<=155' src/apps/message_router/main.go
awk 'NR>=256 && NR<=283' src/apps/meta_db/router_client.go
cd /home/su/.hermes/cache/scratch/go_probe && go run main.go
```
