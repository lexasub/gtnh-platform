# Change: Quest Book icons + lock reasons

## Why

The quest book detail view renders quest requirements and prizes as raw text
(`Reward: item 42 x 1`, `Objective: hold 8 of item 5`), so the player cannot
see what a quest needs or grants at a glance (issue #34). LOCKED quests show
only a gray `[LOCKED]` badge with no explanation of why they are locked.
Both are client-only gaps: the icon-rendering pipeline (`TextureAtlas::GetItemUV`
+ `ItemRegistry::GetName`) and the prerequisite data (`quest_graph.json` via
`QuestData::GetPrerequisites()`) already exist.

A second, structural gap blocks meaningful prize display: quest rewards live
in `quests.csv` as a single flat `reward_item`+`reward_count` pair — and are
currently empty for all 163 quests. That model cannot express GTNH-style
rewards (multiple rewards per quest, choice rewards "pick 1 of N", non-item
rewards). The same applies to requirements: `detect_target` + `target_count`
in CSV cannot express requirement kinds (craft vs obtain-in-machine) or whether
the item is consumed on completion. Both rewards and requirements move to
dedicated JSON files.

## What Changes

- **Reward data moves from `quests.csv` to `data/quests/quest_rewards.json`**
  — a structured reward model: multiple rewards per quest, choice rewards
  (`choice_of`), non-item types (experience/special — already supported by
  MetaDB's `player_quest_rewards.reward_type`). No conditions in this change.
  Consumers: MetaDB (reward resolution on completion) and the client quest
  book (icon rendering).
- **Requirement data moves from `quests.csv` to
  `data/quests/quest_requirements.json`** — structured requirement model:
  kind (`craft` — must craft, `obtain` — must hold, `place` — must place,
  `machine` — must obtain in a machine) and a `consume` flag (item is taken
  away on completion vs. kept). `quests.csv` keeps only `cost_item`/
  `cost_count`/`cooldown` for EXCHANGE quests.
- **Per-quest `auto_complete` flag** in `quest_requirements.json`: quests with
  `auto_complete: true` complete instantly when their requirement is met
  (existing detection paths: craft event, block placement, inventory check on
  quest book open, machine output); quests with `auto_complete: false`
  transition to AVAILABLE and require the manual "Complete" button.
- **`machine`-kind detection** (server-side): when a player obtains an item
  from a machine's output, matching quests with kind `machine` auto-complete
  (subject to `auto_complete` and prerequisites). Renders the machine icon in
  the quest book.
- **Requirement icons** in the quest detail view:
  - `obtain` / `craft` / `place` / `machine` — icon of the target item, with
    the requirement kind, quantity, and (for `obtain`) have/need counts
  - `consume=true` — "taken on completion" badge; `consume=false` — "kept"
  - EXCHANGE — icon of `costItemId` with count
- **Prize icons**: each quest's rewards (from `quest_rewards.json`) render as
  item icons + quantity + item name. Choice rewards show all options.
- **Icon provenance**: icons resolve through the existing registry chain
  (`TextureAtlas::GetItemUV`: item_icons.csv → block_faces.csv → default UV);
  names via `ItemRegistry::GetName`. No hardcoded placeholder textures.
- **Lock reason panel**: for LOCKED quests the detail view lists why — unmet
  prerequisite quests (title + current status) and/or a locked-era gate
  ("Era X is locked — complete Era Y first").
- **Lock-reason computation** extracted to `quest_lib` as a pure, testable
  function (client-only consumption; no wire change).
- **Quest data normalization** — see "What Changes" above. *Assumption:
  reward/requirement assignment is content work; the change seeds reasonable
  values for the base era and leaves later-era data to the owner.*

## Impact

- Affected specs: `questbook` (ADDED requirements)
- Affected code:
  - `data/quests/quest_rewards.json` (new — reward definitions)
  - `data/quests/quest_requirements.json` (new — requirement definitions)
  - `data/quests/quests.csv` (requirement/reward columns dropped; EXCHANGE
    cost/cooldown kept)
  - `src/services/meta_db/definitions.go` (load rewards + requirements from
    JSON instead of CSV)
  - `src/services/simulation_core/Quest/QuestManager.cpp` (`auto_complete`
    gating, `machine`-kind detection)
  - `src/services/game_client/UI/Windows/player/QuestBookWindow.cpp/.h`
  - `src/libs/quest_lib/` (reward/requirement loading, lock-reason helper +
    tests)
- Coordination: `questbook-client-polish` (active change) also edits
  `QuestBookWindow.cpp/.h` — changes must rebase/coordinate on that file.
- No wire/protocol changes: `quest.fbs` / `gateway.fbs` / `GatewayMsg`
  untouched — all data is local files + existing `QuestDef` fields.
- **Choice-reward redemption UX** (player picking an option in-game) is out of
  scope — this change renders choice options; the pick flow is a future change.
