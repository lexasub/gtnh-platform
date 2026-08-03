## 1. Wire Contract & Parsing (parsing landed in 158038a)

- [x] 1.1 `updateQuestStatus()` → FlatBuffers deserialization — **DONE** (`QuestBookWindow.cpp:202` `applyQuestStatus`, commit `158038a`)
- [x] 1.2 `OnNetworkUpdate()` uses `GatewayMsg` enum constants, not hardcoded `19` — **DONE** (`QuestBookWindow.cpp:213`, commit `158038a`)
- [ ] 1.3 Correct `gateway.fbs` GatewayPayload union + header comment: quest entries numbered 20/21/22 to match the live wire (`gateway.h`, `NetClient.h`). Note the `// TODO move to protocol` consolidation as the long-term fix.

## 2. Notifications (client visuals)

- [ ] 2.1 `QuestUnlockNotification` (20) — banner/toast + highlight newly unlocked quests in the quest list (dispatch already exists; visuals don't)
- [ ] 2.2 `QuestCompletedNotification` (21) — banner/toast + reward info (reward item/count)

## 3. Era Progression UI

- [ ] 3.1 Completion ratio badges on era tabs (e.g. "3/12")
- [ ] 3.2 Era lock/unlock visual state — era locked until all quests in the preceding era COMPLETED; locked tabs dimmed, non-selectable; newly unlocked era gets a brief highlight

## 4. Client→Server Routing

- [ ] 4.1 `NetClient::SendQuestGet` — request player quest progress → gateway → `meta_db.quest.get`; response arrives as `QuestProgressUpdate` (20)
- [ ] 4.2 `NetClient::SendQuestSet` + gateway forward to `meta_db.quest.set` — transport only; server-authoritative status validation belongs to `manual-completion`, this route SHALL NOT become the completion path

## 5. Deferred

- [ ] ~~2.1 manual completion button~~ → **DEFERRED** to `manual-completion` (server-authoritative: `QuestCompleteRequest` protocol + `QuestManager::completeQuest()` + reward→inventory; reward delivery blocked on `questbook-reward-inventory`)
