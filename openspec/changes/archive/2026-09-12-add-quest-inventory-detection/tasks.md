# Tasks: Questbook Inventory Detection

Status: **implemented** (archived 2026-09-13).

## 1. Implementation
- [x] 1.1 Add `DetectionType::INVENTORY` to `src/libs/quest_lib/QuestTypes.h`
- [x] 1.2 Add `QuestDef::targetCount` parsed from the `target_count` CSV column
- [x] 1.3 Implement `QuestManager::checkInventory(playerId, slots)` with one-step completion
- [x] 1.4 Add `QuestBookOpen` wire message (`kQuestBookOpen = 33`) + gateway forward on `quest.book.open`
- [x] 1.5 Client sends `QuestBookOpen` on quest book open; quest book shows "Have X / N" objective
- [x] 1.6 Quest data rows carry `target_count`