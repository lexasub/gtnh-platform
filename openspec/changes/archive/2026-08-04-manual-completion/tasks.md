## 1. Protocol

- [x] 1.1 Add `QuestCompleteRequest { player_id:uint64; quest_id:uint32 }` to `quest.fbs`; add to `GatewayPayload` union in `gateway.fbs`
- [x] 1.2 Regenerate: CMake flatc (C++, per-service `*_fbs` targets) + `flatc --go` for `src/protocol/generated/go/`
- [x] 1.3 Add `kQuestCompleteRequest` to `GatewayMsg` in `gateway.h` and `NetClient.h`

## 2. Gateway

- [x] 2.1 Forward `QuestCompleteRequest` → publish `quest.complete.request` (add to gateway ctrl switch)

## 3. SimulationCore

- [x] 3.1 Subscribe `quest.complete.request` in `SimCoreMessageHandler` → `QuestManager::completeQuest()`
- [x] 3.2 `completeQuest(playerId, questId)` — validate current status (reject LOCKED/COMPLETED/unknown) and prereqs via `QuestGraph::CanComplete()` (**verified**: `questGraph` is Init'd in `main.cpp:327` and held by QuestManager — reuse, no new graph wiring)
- [x] 3.3 On acceptance: transition AVAILABLE→COMPLETED; publish `quest.completed` + `quest.progress.updated`; run `QuestGraph::NewlyAvailable()` and publish `quest.unlocked`
- [x] 3.4 Reward grant — via `quest.completed` → MetaDB `HandleQuestCompleted` → `RedeemPlayerQuestReward()` grants to inventory (unblocked by `questbook-reward-inventory`, GTNH-8tw). `distributeRewards()` kept for audit logging.

## 4. Client

- [x] 4.1 Render "Complete" button in detail view when status is AVAILABLE
- [x] 4.2 `NetClient::SendQuestComplete` + click handler; update local status only on server confirmation (`QuestCompletedNotification` / progress update)

## 5. Tests

- [x] 5.1 Test: AVAILABLE quest with prereqs met → COMPLETED + reward granted (`quest.completed` published; MetaDB grant covered by GTNH-8tw Go tests)
- [x] 5.2 Test: LOCKED / already-COMPLETED / unknown quest rejected; status and rewards unchanged
- [x] 5.3 Test: re-completion does not double-grant (idempotency — `completeQuest` rejects COMPLETED; `player_quest_rewards.redeemed` backstop)
