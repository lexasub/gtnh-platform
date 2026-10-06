# Tasks: add-multiplayer-foundation
## Verification 2026-10-06: gateway sessions landed. Per-task status below says what a test actually asserts; unmarked items are untouched.

Evidence: `test/integration/multiplayer_test.go` (two live clients per test), `src/engine/net/include/gtnh/net/client_session.h` + `src/client_session.cpp`.

## 1. Gateway sessions
- [x] 1.1 `ClientSession` + `SessionRegistry` landed in **gtnh-net** (`client_session.h/.cpp`), not in gateway: sessions, per-session ids, interest, pending chunks and request correlation are transport state. Gateway owns the registry and routes.
- [~] 1.2 Partial: accept creates the session and the bulk socket pairs by the session id the client was welcomed with. NOT landed: the AWAIT_BULK→ACTIVE→CLOSED state machine and the orphan-bulk timeout — an unpaired bulk socket is held open until the client closes it.
- [x] 1.3 The `send_to_client_*` singletons are gone, replaced by `send_to_session_*` / `send_to_player_ctrl` / `broadcast_*`. N=1 stays green (multiplayer_test.go + gateway_test.go).
- [x] 1.4 Ctrl close removes exactly that session and publishes `player.left` for that player only; `TestGateway_DisconnectDoesNotAffectOtherSession` covers it.

## 2. Identity & handshake
- [~] 2.1 Landed as `SessionWelcome{session_id, player_id}` + `SessionHello{session_id}` + `PlayerJoined.nickname` in `core.fbs` (types 52/53 in `GatewayMsg.h`). NOT landed: a client-supplied `ClientHello` with a desired id/nickname — identity is server-assigned, so the client proposes nothing. The bulk pairing token IS the server-issued session id.
- [x] 2.2 Canonical id assignment with collision resolution in `SessionRegistry::create_session`; `player.joined` carries the id and nickname.
- [x] 2.3 `gtnh::net::admit_intent` is the single admission point; every ctrl frame claiming a non-zero foreign player_id is dropped with a log.
- [x] 2.4 `invState_.player_id = 1` is gone — GameClient adopts the welcomed id, NetClient sends `SessionHello` on every bulk (re)connect.

## 3. Addressed replies
- [~] 3.1 Correlation landed per session (`SessionRegistry::remember_request`/`claim_request`, keyed by request_id → session list). NOT landed: a TTL — an unanswered request_id stays mapped until its session closes.
- [~] 3.2 Partial: `player.inventory.update`, `pipe.contents.response`, `quest.exchange.response`, `quest.exchange.cooldown.response` and `player.position.load` are routed to their owner. Still broadcast: craft/grid, machine-slot, tool-action and multiblock replies — their payloads carry no `player_id` and their requests carry no `request_id`, so routing them needs a protocol change in SimCore (out of this change's write scope).
- [x] 3.3 `world.blocks.changed` is relayed to every session except the author's; `TestGateway_BlockChangeIsNotEchoedToSource` pins the exclusion.

## 4. Per-session interest
> **Blocked by 4.0 (gp-0yf).** Gateway interest management cannot be wired up
> before this section, for two reasons 4.1/4.2 do not yet name:
>
> - **The client has no position channel.** `gateway.on_client_message`
>   (main.cpp:84-95) drops `MOVE`/`UNLOAD` as flood control, so
>   `last_x_/last_y_/last_z_` are only ever the saved spawn point
>   (gateway.cpp:108-110) or whatever a `SetBlockAction` clicked
>   (gateway.cpp:544, 568). An interest center needs a real position stream;
>   filtering on a stale one silently drops every chunk outside the radius.
> - **A dropped chunk has no recovery path.** `on_router_publish` forwards
>   chunk data unconditionally, and the client cannot re-request a chunk it was
>   never sent. Until a filtered send also publishes a `chunk.requests` entry
>   for what it dropped, the filter turns "render distance" into "holes in the
>   world". Whoever implements 4.1 must add that re-request, or keep the
>   filter unshipped.
>
> `PlayerInterest::ShouldSendChunk` (gateway.h) and the filter site
> (gateway.cpp, the `world.chunk.loaded.compressed` branch) both already
> exist; the blockers are above, not the predicate.

- [ ] 4.0 Client reports its own position on a throttled channel; Gateway
      consumes it into the session instead of dropping MOVE (main.cpp:84-95)
- [x] 4.1 `PlayerInterest` moved into the session (gtnh-net). The radius filter itself is still NOT wired — see 4.0's blockers. What landed instead is request-scoped delivery: a chunk is pushed only to the sessions that asked for that coordinate (`SessionRegistry::pending_chunks`), which isolates clients without a stale centre and without unrecoverable drops.
- [x] 4.2 `chunk.requests` is published with the session's player_id; the player's own `CHUNK_REQUEST` frames are re-attributed to the session server-side instead of being forwarded verbatim.
- [~] 4.3 `player.position.load` is routed to the owning session. NOT landed: ChunkLoadManager still sends UNLOAD with player_id=0 (that action is dropped as flood control anyway, see gp-rxry).

## 5. Visibility
- [ ] 5.1 Position snapshots for remote players (decide reuse `EntitySnapshot` vs new table per design open question)
- [ ] 5.2 SimCore publishes player position events from `player.joined`/position updates; gateway filters by interest

## 6. Tests & baseline
- [x] 6.1 Two simultaneous clients with handshake, unique ids and per-session routing — `TestGateway_SessionsAreIndependent`, `TestGateway_ChunkStreamIsPerSession`.
- [ ] 6.2 No test yet for identical request_ids across sessions. The correlation map is keyed per session, but "two clients, same request_id" is unasserted; add it with the first craft/tool RPC correlation (3.2).
- [x] 6.3 `TestGateway_BlockChangeReachesOtherClient` + `TestGateway_BlockChangeIsNotEchoedToSource`.
- [x] 6.4 `TestGateway_DisconnectDoesNotAffectOtherSession`.
- [ ] 6.5 Throughput baseline harness N=2..8, record msg/s artifact (input to roadmap cluster B)
- [x] 6.6 `docs/gateway-headless-client.md` gained a "Session handshake (required, multi-client)" section; `gateway_cli` prints the welcome and adopts the assigned id.

## 7. Docs & roadmap
- [ ] 7.1 ROADMAP: add section A.1 (multiplayer foundation) before B; close «Gateway single-client» known-issue row
- [ ] 7.2 Update openspec spec deltas status after review; archive obsolete parts of multiplayer-sync requirements replaced by deltas
