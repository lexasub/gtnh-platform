# Tasks: add-multiplayer-foundation
# Verification 2026-09-21: all tasks pending – no ClientSession, no handshake, singleton gateway remains; see evidence in verification report.
## 1. Gateway sessions
- [ ] 1.1 Introduce `ClientSession` (ctrl+bulk conns, player_id, nickname, interest, last_pos) and session map/indices in `gateway.h/.cpp`
- [ ] 1.2 Accept path: create session, pair ctrl+bulk via bulk_token, reject orphan bulk after timeout (state machine AWAIT_BULK→ACTIVE→CLOSED)
- [ ] 1.3 Replace singleton `client_ctrl_`/`client_bulk_` sends with per-session fanout helpers; keep N=1 behavior green
- [ ] 1.4 Per-session teardown: publish `player.left` for that player only; flush last position; cleanup maps/indices

## 2. Identity & handshake
- [ ] 2.1 Protocol: `ClientHello` (desired id, nickname, bulk_token) + nickname field on `PlayerJoined`/`PlayerLeft` in `core.fbs` (backward-compatible)
- [ ] 2.2 Gateway: canonical id assignment, collision resolution, `player.joined` with nickname
- [ ] 2.3 Validate per-session player_id on inbound actions; drop spoofed ids
- [ ] 2.4 GameClient: remove hardcoded `invState_.player_id = 1` (GameClient.cpp:269), adopt server-issued id; NetClient handshake message

## 3. Addressed replies
- [ ] 3.1 Correlate request_ids per session (`session_id|request_id` map with TTL) in gateway
- [ ] 3.2 Route inventory/quest/craft/machine/tool replies by payload player_id or correlation to owning session
- [ ] 3.3 `world.blocks.changed`: exclude only source session; relay to others (per-session `source_player_id` check)

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
- [ ] 4.1 Move `PlayerInterest` into session; per-session `ShouldSendChunk` filtering for `world.chunk.loaded.compressed`
- [ ] 4.2 `chunk.requests` published with session player_id (no more player_id=0: gateway.cpp:131, gateway.cpp:488-492)
- [ ] 4.3 Route `player.position.load` to owning session; ChunkLoadManager sends unload/requests with session id

## 5. Visibility
- [ ] 5.1 Position snapshots for remote players (decide reuse `EntitySnapshot` vs new table per design open question)
- [ ] 5.2 SimCore publishes player position events from `player.joined`/position updates; gateway filters by interest

## 6. Tests & baseline
- [ ] 6.1 Headless harness: two simultaneous clients, handshake, unique ids, response isolation
- [ ] 6.2 Test: identical request_id collision isolation across sessions
- [ ] 6.3 Test: peer block change received by other client, no echo to source
- [ ] 6.4 Test: one disconnect does not affect other session (player.left per player)
- [ ] 6.5 Throughput baseline harness N=2..8, record msg/s artifact (input to roadmap cluster B)
- [ ] 6.6 Update `tools/gateway_cli/` / `test/integration/` docs for multi-client usage

## 7. Docs & roadmap
- [ ] 7.1 ROADMAP: add section A.1 (multiplayer foundation) before B; close «Gateway single-client» known-issue row
- [ ] 7.2 Update openspec spec deltas status after review; archive obsolete parts of multiplayer-sync requirements replaced by deltas
