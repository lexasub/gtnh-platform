# Change: Questbook Advanced Detection Handlers

## Why
QuestManager detects CRAFT and BLOCK_PLACED, but TOOL_CHARGED and SIDE_CONFIGURED detection types have no handlers, `QuestUnlocked` events are not published on `quest.unlocked`, and QuestGraph (`CanComplete()`/`NewlyAvailable()`) is initialized but not used — completion checking still loops inline over prerequisites.

## What Changes
- Replace inline prereq checking with `QuestGraph::CanComplete()` / `NewlyAvailable()`.
- TOOL_CHARGED detection handler — listen for tool charge events, call QuestManager.
- SIDE_CONFIGURED detection handler — listen for wrench/side config events, call QuestManager.
- Publish `QuestUnlocked` events after each completion via `QuestGraph::NewlyAvailable()`.

## Impact
- Affected specs: questbook-detection-handlers (new)
- Affected code:
  - `src/services/simulation_core/Quest/QuestManager.cpp` — prereq refactor (lines 103, 181), new handlers, unlock publish
  - `src/libs/quest_lib/QuestGraph.h/.cpp` — ensure `CanComplete()`/`NewlyAvailable()` wired
