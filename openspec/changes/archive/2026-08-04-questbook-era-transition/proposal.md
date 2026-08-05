# Change: Questbook Era Transition

## Why
QuestGraph has `IsEraComplete()` but it is not wired: `BuildQuestEraMap()` is missing and no era-transition message type exists. When a player completes all quests in an era, the client should learn the next era is unlocked.

## What Changes
- `quest::QuestData::BuildQuestEraMap()` — quest_id → Era lookup. ✅ Done (`QuestData.cpp`)
- Wire `IsEraComplete()` after each quest completion in `QuestManager`. ✅ Done — `maybePublishEraTransition()` from `completeQuest`/`checkCraftCompletion`/`checkBlockAction`; one-shot per era via `completedEras_`
- Publish era transition event/notification when a full era is done. ✅ Done — `EraTransitionNotification` in `quest.fbs`, topic `quest.era.transition`, forwarded by Gateway (`kQuestEraTransition=25`), client auto-selects/highlights the next era tab

## Implementation Notes
- One-shot semantics: an era transition is published exactly once per player per era (tracked in `QuestManager::completedEras_`); `loadProgress` rebuilds the set so re-login does not re-publish
- `next_era` equals `completed_era` when the final era (ADMINISTRATOR) completes
- Client: `applyEraTransition()` marks the completed era with "✓", stores the next era for auto-selection on next render (transition may arrive before first draw); `renderEraTabs()` highlights the newly available era
- Test: `QuestManager_eraTransition_published_once` (simcored_test) — completes all 9 VAGRANT quests, asserts exactly one `quest.era.transition` with completed=VAGRANT(0), next=APPRENTICE(1)

## Impact
- Affected specs: questbook-era-transition (new)
- Affected code:
  - `src/libs/quest_lib/QuestData.h/.cpp`
  - `src/services/simulation_core/Quest/QuestManager.cpp`
  - `src/protocol/quest.fbs` — era transition message type
  - `src/services/gateway/gateway.{h,cpp}`, `main.cpp` — topic forwarding
  - `src/services/game_client/Network/NetClient.{h,cpp}`, `UI/Windows/player/QuestBookWindow.{h,cpp}`
