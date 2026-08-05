# Change: Questbook Advanced Detection Handlers

## Why
QuestManager detects CRAFT and BLOCK_PLACED but has no handlers for TOOL_CHARGED and SIDE_CONFIGURED. Its detection paths (`checkCraftCompletion()`, `checkBlockAction()`) also evaluate prerequisites inline instead of via QuestGraph, and they neither cascade unlocks nor publish `QuestUnlocked`. Separately, `onPlayerJoined()` seeds every quest LOCKED, so the "make AVAILABLE" branches inside the detection paths are unreachable — a fresh player can never complete a quest through detection. This change unifies detection on QuestGraph with one-step completion semantics and adds the two missing handlers.

## What Changes
- **One-step completion semantics (all detection types)**: LOCKED/AVAILABLE quest with met prerequisites → COMPLETED directly on detection, no intermediate AVAILABLE gate. Fixes the dead-detection bug above.
- Replace inline prereq checking in `checkCraftCompletion()`/`checkBlockAction()` with `QuestGraph::CanComplete()`; publish unlock batches via `QuestGraph::NewlyAvailable()` on `quest.unlocked` after detection-path completions.
- TOOL_CHARGED detection handler — implement the `CHARGE_ITEM` case in `ToolActionHandler` (currently a logging stub): when the tool is at full charge, call `QuestManager::checkToolCharged()`. Server-side only; no client emits CHARGE_ITEM yet.
- SIDE_CONFIGURED detection handler — extend `WrenchCycleResult` with `machine_id`; after a successful `cycleFace()` in `WrenchActionHandler`, call `QuestManager::checkSideConfigured()` with the machine id (skip hatches where `machine_id == 0`).
- Add quest data: TOOL_CHARGED → ULV drill `1111:00:0` (prereq 18), SIDE_CONFIGURED → heat furnace `1110:00:0` (prereq 22) in `quests.csv` + `quest_graph.json`.

## Impact
- Affected specs: questbook-detection-handlers (new)
- Affected code:
  - `src/services/simulation_core/Quest/QuestManager.{h,cpp}` — one-step detection refactor, `checkToolCharged()`/`checkSideConfigured()`, unlock cascade + publish on detection paths
  - `src/services/simulation_core/Actions/WrenchHandler.{h,cpp}` — add `machine_id` to `WrenchCycleResult`
  - `src/services/simulation_core/Actions/WrenchActionHandler.cpp` — call `checkSideConfigured()` after `cycleFace()` succeeds
  - `src/services/simulation_core/Actions/ToolActionHandler.cpp` — implement `CHARGE_ITEM`
  - `data/quests/quests.csv`, `data/quests/quest_graph.json` — two new quests
  - `src/libs/quest_lib/QuestGraph.{h,cpp}` — already exposes `CanComplete()`/`NewlyAvailable()`; no change expected
