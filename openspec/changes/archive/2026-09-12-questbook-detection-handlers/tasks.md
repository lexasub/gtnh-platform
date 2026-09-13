## 1. QuestGraph Wiring

- [x] 1.1 Replace inline prerequisite checking in detection paths with QuestGraph — `QuestManager::checkCraftCompletion()` and `checkBlockAction()` loop + check prerequisites inline (QuestManager.cpp:226-235, 307-316). Refactor to use `QuestGraph::CanComplete()` + `QuestGraph::NewlyAvailable()`.
- [x] 1.2 One-step completion semantics for ALL detection types — LOCKED/AVAILABLE + `CanComplete()==true` → COMPLETED directly (no intermediate AVAILABLE gate). Fixes the dead-detection bug: `onPlayerJoined()` seeds every quest LOCKED (QuestManager.cpp:180-182), so the old "make AVAILABLE" branch in detection paths was unreachable. Publish `QuestCompleted` + evaluate era transition (`maybePublishEraTransition`) + unlock cascade + `quest.unlocked` for detection-path completions.

## 2. Detection Handlers

- [x] 2.1 Implement TOOL_CHARGED detection handler — implement the `CHARGE_ITEM` case in `ToolActionHandler::handle()` (currently a logging stub, ToolActionHandler.cpp:39): inject QuestManager, on tool at full charge call `checkToolCharged(playerId, itemId)`. Server-side only for now — no client emits CHARGE_ITEM yet (deferred to a separate change).
- [x] 2.2 Implement SIDE_CONFIGURED detection handler — add `machine_id` (packed block id) to `WrenchCycleResult` (WrenchHandler.h:10-15; `cycleFace()` already has the entity, SimulationEngine.cpp:318). After a successful `cycleFace()` in `WrenchActionHandler::handle()` (WrenchActionHandler.cpp:40), call `checkSideConfigured(playerId, machineId)`. Skip when `machine_id == 0` (multiblock hatches, WrenchHandler.cpp:40-67).

## 3. Unlock Events

- [x] 3.1 Publish `QuestUnlocked` events after detection-path completions — call `QuestGraph::NewlyAvailable()` and publish results on the `quest.unlocked` topic. (The manual `completeQuest` path already does this, QuestManager.cpp:149-158.)

## 4. Quest Data

- [x] 4.1 Add TOOL_CHARGED quest (id 40, detectTarget `1111:00:0` ULV drill, prereq 18) + SIDE_CONFIGURED quest (id 41, detectTarget `1110:00:0` heat furnace, prereq 22) to `data/quests/quests.csv` with rewards (ids 40/41 currently unused; `reward_item` is a plain numeric uint16 — legacy convention matching all 39 existing quests, not a packed id; used 60/61).
- [x] 4.2 Add both quests to `data/quests/quest_graph.json` with positions and prerequisites (wrench quest 22 as prereq for side config).

## 5. Tests

- [x] 5.1 Add detection tests in `src/services/simulation_core/test/test_quest_manager.cpp`: one-step completion via QuestGraph (LOCKED quest with met prerequisites completes on detection), TOOL_CHARGED completion, SIDE_CONFIGURED completion, `quest.unlocked` publishing from a detection path.
