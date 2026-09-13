## 1. Block Change Broadcast
- [x] 1.1 Document world.blocks.changed → Gateway → all clients flow (spec: Block Change Broadcast)
- [x] 1.2 Verify Gateway broadcasts to all connected clients on block change (gateway.cpp:354 `world.blocks.changed` → `kBlockUpdate` on bulk; source-player skip via `source_player_id`)
- [ ] 1.3 Interest management (only send chunks near player) — NOT implemented: `Gateway::client_interest()` is a nullptr TODO placeholder, full broadcast today. Spec documents current behavior; separate change needed.

## 2. Player Disconnect
- [x] 2.1 Document disconnect handling (gateway.cpp:75 publishes `player.left` after position flush; SimulationCore has NO `player.left` subscriber, so player-bound ECS cleanup is NOT implemented — documented honestly, separate change needed)
- [x] 2.2 Verify SimulationCore continues 20Hz tick without player (sim is a separate process; no disconnect coupling found)
- [x] 2.3 Topic is `player.left`, not `player.disconnected` — spec documents the real topic

## 3. Player Reconnect
- [x] 3.1 Document reconnect flow (TCP connect → `player.joined` → MetaDB push; chunk reload via `world.chunk.loaded.compressed` → `kCompressedChunkData` on bulk)
- [x] 3.2 Verify MetaDB state restoration (gateway.cpp:99-113 publishes `player.joined` eagerly; SimCore has PlayerJoinedHandler; MetaDB pushes inventory)
- [x] 3.3 No separate `player.reconnected` topic exists — reconnects reuse `player.joined`; spec documents actual state sync

## 4. Service Communication Patterns
- [x] 4.1 Document pub/sub pattern (1-to-N fan-out) — spec: Service Communication Patterns
- [x] 4.2 Document RPC pattern (request-response via MessageRouter) — spec: Service Communication Patterns
- [x] 4.3 Document chained pub/sub pattern (event → handler → new event) — spec: Service Communication Patterns