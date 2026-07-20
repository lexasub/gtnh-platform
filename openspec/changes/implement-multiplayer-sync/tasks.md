## 1. Block Change Broadcast
- [ ] 1.1 Document world.blocks.changed → Gateway → all clients flow
- [ ] 1.2 Verify Gateway broadcasts to all connected clients on block change
- [ ] 1.3 Document interest management (only send chunks near player)

## 2. Player Disconnect
- [ ] 2.1 Document disconnect cleanup (free player-bound ECS entities)
- [ ] 2.2 Verify SimulationCore continues 20Hz tick without player
- [ ] 2.3 Document player.disconnected topic broadcast

## 3. Player Reconnect
- [ ] 3.1 Document reconnect flow (TCP connect → welcome → chunk reload)
- [ ] 3.2 Verify MetaDB state restoration (position, inventory)
- [ ] 3.3 Document player.reconnected topic and state sync

## 4. Service Communication Patterns
- [ ] 4.1 Document pub/sub pattern (1-to-N fan-out)
- [ ] 4.2 Document RPC pattern (request-response via MessageRouter)
- [ ] 4.3 Document chained pub/sub pattern (event → handler → new event)
