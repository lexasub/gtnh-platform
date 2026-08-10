## 1. Quest reward data → JSON (`data/quests/quest_rewards.json`)
- [x] 1.1 Design the JSON schema (per design.md): quest id → `rewards` array XOR `choice_of` array; entry fields `type` (`item`/`experience`/`special`), `item` (spec string, packed via `ItemId::pack`), `count`, `value`
- [x] 1.2 Create `data/quests/quest_rewards.json` with seed definitions for base-era (Vagrant) quests; quests without rewards omitted — **seeded ids 1–36 only** (Vagrant era)
- [x] 1.3 quest_lib: add `QuestRewards::LoadJSON()` (nlohmann/json) — parse + validate (rewards XOR choice_of) + pack item specs; unit tests load the shipped JSON

## 2. Quest requirement data → JSON (`data/quests/quest_requirements.json`)
- [x] 2.1 Design the JSON schema (per design.md): quest id → `auto_complete` (bool, default true) + `requirements` array; entry fields `kind` (`craft`/`obtain`/`place`/`machine`), `item` (spec string), `count`, `consume` (bool, default false), `machine` (only for kind `machine`)
- [x] 2.2 Create `data/quests/quest_requirements.json` with seed definitions — **all 163 quests**, incl. `auto_complete: false` variants
- [x] 2.3 quest_lib: add `QuestRequirements::LoadJSON()` (nlohmann/json) — parse + validate kinds + pack item specs; unit tests load the shipped JSON
- [x] 2.4 MetaDB (`definitions.go`): replace CSV reward lookup with `quest_rewards.json` load and CSV requirement lookup with `quest_requirements.json` load; keep cost/cooldown in CSV
- [x] 2.5 Normalize `quests.csv` to the 9-column header (`id,title,description,era,section,cost_item,cost_count,cooldown,target_count`): move INVENTORY quantity into `target_count`; drop `reward_item`/`reward_count` (now sourced from JSON). Quest 27/28/36 uncommitted WIP reorder preserved.
- [x] 2.6 Verify both parsers consistently: `QuestData.cpp LoadCSV` (C++), `definitions.go` (Go)
- [x] 2.7 QuestData: merge JSON requirements into `QuestDef.detectType`/`detectTarget` (kind → DetectionType map, item → detectTarget) so existing QuestManager triggers keep firing; unit test CRAFT quest detect fields survive JSON load

## 3. Auto-complete gating + machine detection (QuestManager.cpp)
- [x] 3.1 Gate completion behind `auto_complete` via `handleQuestMet` (NOT `completeQuestInternal`): true → complete immediately (current behavior); false → transition to AVAILABLE only, require manual `completeQuest`
- [x] 3.2 Add `checkMachineOutput(playerId, machineId, itemId, count)`: matches kind `machine` quests (item + machine), checks prerequisites + `auto_complete`, completes or falls to AVAILABLE
- [x] 3.3 Hook `checkMachineOutput` into the machine output-slot take path (`MachineSlotHandler`); INVENTORY-style fallback when machine attribution missing. `simcored_exec` builds.
- [x] 3.4 Unit tests: auto_complete true/false/default; machine quest completes on output take; fallback path — `simcored_test`: 79 tests/430 checks pass (4 new: autoComplete_false_gates_manual, autoComplete_default_is_true, detection_machine_output, detection_machine_fallback). Note: verified via standalone link (full `ninja` blocked by unrelated in-flight `pipe_network.fbs` flatc break)

## 4. Lock-reason computation in quest_lib
- [x] 4.1 Add `QuestGraph::LockedByPrereqs(questId, current)` → unmet prerequisite quest ids (missing-from-map treated as not-completed)
- [x] 4.2 Add unit tests: unmet prereqs listed, mixed blockers (quest_lib test suite: `LockedByPrereqs`)

## 5. Client quest book UI (`QuestBookWindow.cpp/.h`)
- [x] 5.1 Keep `QuestData` returned from `loadQuestData()` in member + init `QuestGraph` with prereq map; `LockedByPrereqs` resolved during detail render
- [x] 5.2 Add item-icon render helper: `dl->AddImage` with `TextureAtlas::GetTextureHandle().idx` + `GetItemUV(itemId)`, count overlay, hover tooltip; raw-spec fallback when `ItemId::pack` yields 0 (unregistered item)
- [x] 5.3 Replace text-only requirement/reward rows in `renderQuestDetail()` with icon+count rows (requirements by kind incl. MACHINE tooltip/consume, rewards from `quest_rewards.json` incl. all `choiceOf` options via radio + picked-entry display)
- [x] 5.4 Add lock-reason panel in `renderQuestDetail()` for status LOCKED via `questGraph_.LockedByPrereqs` (unmet prereq ids + titles)
- [x] 5.5 Show Complete button for ALL AVAILABLE non-exchange quests (regardless of `auto_complete`); for `auto_complete: true` also show "Completes automatically" hint beside the button. (Revised: button no longer hidden for autoComplete — player can complete manually in any case; server still gatekeeps via `QuestCompletedNotification`.)

## 6. Verification
- [x] 6.1 `cmake-build-debug` ninja build passes (`gameclientd` `simcored_exec`)
- [x] 6.2 `ctest --output-on-failure -j$(nproc)` passes (including new quest_lib + QuestManager tests) — **DONE**: 12/13 ran (toctou disabled), 100% passed, 0 failed. Full `ninja -j5` clean (1 pre-existing warning in RenderBridge.cpp).
- [ ] 6.3 Manual: open quest book, select LOCKED quest with unmet prereqs → blockers listed; select quest with reward → icon+name shown; quest with choice → all options shown; quest with consume=true → "taken" badge; auto_complete=false quest stays AVAILABLE until Complete pressed; machine quest completes on output take; verify no hardcoded placeholder for registered items
