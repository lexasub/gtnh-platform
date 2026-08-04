# Change: Manual Quest Completion (server-authoritative)

## Why
The quest book has no manual completion button. Completion is detection-driven only (craft an item / place a block — `QuestManager::checkCraftCompletion` / `checkBlockAction`). Players cannot claim a quest that is already satisfied, and there is no server-authoritative path to mark a quest complete with reward delivery. A naive client→`quest.set` route would let any client mark any quest COMPLETED with no validation and would skip rewards entirely (MetaDB `HandleQuestSet` persists whatever status the payload carries).

## What Changes
- **Protocol**: add `QuestCompleteRequest` (client→gateway) to `quest.fbs`; add it to the `GatewayPayload` union in `gateway.fbs`; add `kQuestCompleteRequest` to the `GatewayMsg` namespaces.
- **Gateway**: forward `QuestCompleteRequest` to SimulationCore on a new `quest.complete.request` topic.
- **SimulationCore**: `QuestManager::completeQuest(playerId, questId)` — validate status + prereqs via `QuestGraph::CanComplete()`; transition AVAILABLE→COMPLETED; publish `quest.completed` + `quest.progress.updated`; re-run `QuestGraph::NewlyAvailable()` and publish `quest.unlocked` for newly available dependents.
- **Rewards**: grant `rewardItemId`×`rewardCount` into the player inventory via MetaDB `RedeemPlayerQuestReward()` — **blocked on** `questbook-reward-inventory` (which wires redemption to the inventory API).
- **Client**: "Complete" button in the detail view when status is AVAILABLE; optimistic UI that only commits local status on server confirmation.

## Impact
- Affected specs: manual-completion (new)
- Affected code:
  - `src/protocol/quest.fbs`, `src/protocol/gateway.fbs` — new message + union member (regenerate C++ via CMake flatc; Go via flatc --go)
  - `src/services/gateway/gateway.cpp/.h` — forward rule + msg type constant
  - `src/services/simulation_core/Quest/QuestManager.h/.cpp` — `completeQuest()`, reward grant
  - `src/services/meta_db/reward_handlers.go` — reward delivery (depends on `questbook-reward-inventory`)
  - `src/services/game_client/UI/Windows/player/QuestBookWindow.cpp/.h`, `Network/NetClient.cpp/.h` — button + `SendQuestComplete`
- Blocked by: `questbook-reward-inventory` (reward→inventory delivery)
- Related: deferred from `questbook-client-polish` task 2.1
