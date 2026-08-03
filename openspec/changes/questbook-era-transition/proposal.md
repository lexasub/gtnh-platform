# Change: Questbook Era Transition

## Why
QuestGraph has `IsEraComplete()` but it is not wired: `BuildQuestEraMap()` is missing and no era-transition message type exists. When a player completes all quests in an era, the client should learn the next era is unlocked.

## What Changes
- `quest::QuestData::BuildQuestEraMap()` — quest_id → Era lookup.
- Wire `IsEraComplete()` after each quest completion in `QuestManager`.
- Publish era transition event/notification when a full era is done.

## Impact
- Affected specs: questbook-era-transition (new)
- Affected code:
  - `src/libs/quest_lib/QuestData.h/.cpp`
  - `src/services/simulation_core/Quest/QuestManager.cpp`
  - `src/protocol/quest.fbs` — era transition message type
