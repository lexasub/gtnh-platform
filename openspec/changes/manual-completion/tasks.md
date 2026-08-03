## 1. Protocol

- [ ] 1.1 Add `QuestCompleteRequest { player_id:uint64; quest_id:uint32 }` to `quest.fbs`; add to `GatewayPayload` union in `gateway.fbs`
- [ ] 1.2 Regenerate: CMake flatc (C++, per-service `*_fbs` targets) + `flatc --go` for `src/protocol/generated/go/`
- [ ] 1.3 Add `kQuestCompleteRequest` to `GatewayMsg` in `gateway.h` and `NetClient.h`

## 2. Gateway

- [ ] 2.1 Forward `QuestCompleteRequest` → publish `quest.complete.request` (add to gateway ctrl switch)

## 3. SimulationCore

- [ ] 3.1 Subscribe `quest.complete.request` in `SimCoreMessageHandler` → `QuestManager::completeQuest()`
- [ ] 3.2 `completeQuest(playerId, questId)` — validate current status (reject LOCKED/COMPLETED/unknown) and prereqs via `QuestGraph::CanComplete()`
- [ ] 3.3 On acceptance: transition AVAILABLE→COMPLETED; publish `quest.completed` + `quest.progress.updated`; run `QuestGraph::NewlyAvailable()` and publish `quest.unlocked`
- [ ] 3.4 Reward grant — `distributeRewards()` → actual inventory delivery via MetaDB (BLOCKED on `questbook-reward-inventory`; `RedeemPlayerQuestReward()` currently only marks redeemed)

## 4. Client

- [ ] 4.1 Render "Complete" button in detail view when status is AVAILABLE
- [ ] 4.2 `NetClient::SendQuestComplete` + click handler; update local status only on server confirmation (`QuestCompletedNotification` / progress update)

## 5. Tests

- [ ] 5.1 Test: AVAILABLE quest with prereqs met → COMPLETED + reward granted
- [ ] 5.2 Test: LOCKED / already-COMPLETED / unknown quest rejected; status and rewards unchanged
- [ ] 5.3 Test: re-completion does not double-grant (idempotency via `player_quest_rewards.redeemed`)
