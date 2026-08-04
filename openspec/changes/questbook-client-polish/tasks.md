## 1. Wire Contract & Parsing (parsing landed in 158038a)

- [x] 1.1 `updateQuestStatus()` → FlatBuffers deserialization — **DONE** (`QuestBookWindow.cpp:202` `applyQuestStatus`, commit `158038a`)
- [x] 1.2 `OnNetworkUpdate()` uses `GatewayMsg` enum constants, not hardcoded `19` — **DONE** (`QuestBookWindow.cpp:213`, commit `158038a`)
- [ ] 1.3 Correct `gateway.fbs` GatewayPayload union + header comment: quest entries numbered 20/21/22 to match the live wire (`gateway.h`, `NetClient.h`). Note the `// TODO move to protocol` consolidation as the long-term fix.

## 2. Notifications (client visuals)

- [ ] 2.1 `QuestUnlockNotification` (20) — banner/toast + highlight newly unlocked quests in the quest list (dispatch already exists; visuals don't)
- [ ] 2.2 `QuestCompletedNotification` (21) — banner/toast + reward info (reward item/count)

## 3. Era Progression UI

- [ ] 3.1 Completion ratio badges on era tabs (e.g. "3/12")
- [ ] 3.2 Era lock/unlock visual state — locked by default (dimmed, non-selectable); released only when the server signals the era unlocked (era-transition notification per `questbook-era-transition`). Completion-ratio badge on the tab is client-derived from received statuses and SHALL still render while locked (safe default: locked until told otherwise)

## 4. Deferred / Removed

- [x] ~~4.1 client `quest.get` resync~~ — **REMOVED** — initial progress is already server-pushed on join (`PlayerJoinedHandler` → `meta_db.quest.get` → `HandleQuestGet` → gateway → client). A client-initiated resync for reconnect/refresh is a separate feature, out of scope.
- [x] ~~quest.set client route~~ — **REMOVED** — no client write-path consumer (manual completion uses `QuestCompleteRequest` in `manual-completion`, see 5); a `quest.set` route would be an unvalidated status-mutation hole.

## 5. Deferred

- [ ] ~~2.1 manual completion button~~ → **DEFERRED** to `manual-completion` (server-authoritative: `QuestCompleteRequest` protocol + `QuestManager::completeQuest()` + reward→inventory; reward delivery blocked on `questbook-reward-inventory`)
