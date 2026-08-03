## 1. QuestGraph Wiring

- [ ] 1.1 Replace inline prereq checking with QuestGraph — `QuestManager::checkCraftCompletion()` and `checkBlockAction()` currently loop + check prereqs inline (lines 62-71, 140-149). Refactor to use `QuestGraph::CanComplete()` + `QuestGraph::NewlyAvailable()`

## 2. Detection Handlers

- [ ] 2.1 Implement TOOL_CHARGED detection handler — listen for tool charge events, call QuestManager
- [ ] 2.2 Implement SIDE_CONFIGURED detection handler — listen for wrench/side config events, call QuestManager

## 3. Unlock Events

- [ ] 3.1 Publish `QuestUnlocked` events — after each completion, call `QuestGraph::NewlyAvailable()` and publish results on `quest.unlocked` topic
