## 1. Data Layer

- [ ] 1.1 Add `quest::QuestData::BuildQuestEraMap()` — return `std::unordered_map<uint32_t, quest::Era>` for quest_id → era lookup

## 2. SimulationCore

- [ ] 2.1 Wire era transition — after completing a quest, call `QuestGraph::IsEraComplete()`; publish era transition event if full era done
- [ ] 2.2 Add era-transition message type to `quest.fbs` (if not present) and publish on transition

## 3. Client

- [ ] 3.1 Show new era tab as active/available on era transition
