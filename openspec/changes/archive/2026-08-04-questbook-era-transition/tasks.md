## 1. Data Layer

- [x] 1.1 Add `quest::QuestData::BuildQuestEraMap()` — return `std::unordered_map<uint32_t, quest::Era>` for quest_id → era lookup (`QuestData.h:29-31`, `QuestData.cpp:249-257`)

## 2. SimulationCore

- [x] 2.1 Wire era transition — after completing a quest, call `QuestGraph::IsEraComplete()`; publish era transition event if full era done (`QuestManager.cpp`: `maybePublishEraTransition()`, called from `completeQuest`/`checkCraftCompletion`/`checkBlockAction`; one-shot per era per player via `completedEras_`; `loadProgress` rebuilds completed eras)
- [x] 2.2 Add era-transition message type to `quest.fbs` (if not present) and publish on transition (`EraTransitionNotification` table; published on topic `quest.era.transition`)

## 3. Client

- [x] 3.1 Show new era tab as active/available on era transition (`QuestBookWindow.cpp`: `applyEraTransition()` — marks completed era ✓, auto-selects next era tab; `renderEraTabs()` highlights newly available era)
