## Context

Quest rewards are currently a flat `reward_item`+`reward_count` pair in
`data/quests/quests.csv` (columns 8-9), resolved by MetaDB on completion via
`loadQuestDefinitions()` in `definitions.go` (Go `csv.Reader`) and rendered by
the client quest book (`QuestBookWindow` via `quest_lib::QuestData::LoadCSV`).
The columns are empty for all 163 quests. Quest requirements are the flat
`detect_type`+`detect_target`+`target_count` trio in the same CSV — they cannot
distinguish requirement kinds (craft vs. obtain-in-machine) or whether the item
is consumed on completion.

MetaDB's `player_quest_rewards` table already models the target richness:
`reward_type` (`item`/`experience`/`special`), `reward_id`, `reward_count`,
`reward_value`, `metadata` — so a structured reward definition can map onto
the existing persistence without schema changes.

Consumers:
- **MetaDB (Go)** — resolves rewards on `quest.completed` (currently via CSV).
- **GameClient (C++ quest_lib)** — renders requirement + reward icons in the
  quest book.
- **SimulationCore** — does not resolve rewards (`distributeRewards()` only
  logs + publishes the completion event); unaffected by data move.

## Goals / Non-Goals

- Goals:
  - Rewards definable per quest: multiple entries, choice groups, non-item
    types.
  - Requirements definable per quest: kind (`craft`/`obtain`/`place`/`machine`)
    and `consume` flag.
  - Per-quest `auto_complete` flag: instant completion vs. manual button.
  - Single source of truth for reward + requirement definitions, readable by
    both Go (encoding/json) and C++ (nlohmann/json — already a quest_lib
    dependency).
  - Client renders all requirement/reward options as registry-sourced icons;
    no hardcoded placeholders.
- Non-Goals:
  - Choice **redemption** UX (player picking an option) — future change; this
    change only defines + renders options.
  - **Conditions** on rewards/requirements — explicitly out of scope for this
    change; schema has no condition field.
  - Moving `cost_item`/`cost_count`/`cooldown` (EXCHANGE) out of CSV — kept
    in CSV.

## Decisions

- **Decision: two dedicated JSON files — `data/quests/quest_rewards.json` and
  `data/quests/quest_requirements.json`.**
  Separate from `quests.csv` (flat, column-bound) and `quest_graph.json` (DAG
  topology, no reward concern). One quest → one JSON object keyed by quest id,
  mirroring the existing per-quest file convention. Two files keep concerns
  separate (rewards grant on completion; requirements gate the objective) and
  let each be loaded by the consumers that need it.

  `quest_rewards.json` schema:
  ```json
  {
    "1": {
      "rewards": [
        { "type": "item", "item": "0:10:11:2", "count": 4 }
      ]
    },
    "2": {
      "choice_of": [
        { "type": "item", "item": "0:11110:3", "count": 1 },
        { "type": "item", "item": "0:11110:4", "count": 1 }
      ]
    },
    "3": {
      "rewards": [
        { "type": "experience", "value": 50 },
        { "type": "item", "item": "0:1110:5", "count": 2 }
      ]
    }
  }
  ```

  Entry fields: `type` (`item` | `experience` | `special`), `item`
  (hierarchical spec string, packed via existing `ItemId::pack` /
  `packItemSpec`), `count` (uint8), `value` (float, for experience).
  Quest entry: `rewards` (list, all granted) XOR `choice_of` (list, pick one).

  `quest_requirements.json` schema:
  ```json
  {
    "1": {
      "auto_complete": true,
      "requirements": [
        { "kind": "obtain", "item": "0:10:11:2", "count": 8, "consume": false }
      ]
    },
    "2": {
      "auto_complete": false,
      "requirements": [
        { "kind": "craft", "item": "0:10:00:0", "count": 4, "consume": false },
        { "kind": "machine", "item": "0:1110:3", "count": 1, "consume": true,
          "machine": "0:1110:1" }
      ]
    }
  }
  ```

  Quest entry: `auto_complete` (bool, default true — complete instantly when
  the requirement is met) + `requirements` (array).
  Entry fields: `kind` (`craft` — must craft, `obtain` — must hold,
  `place` — must place, `machine` — must obtain in a machine, plus any
  existing `DetectionType` string e.g. `side_configured`), `item`
  (spec string), `count` (uint8), `consume` (bool, default false — item taken
  on completion), `machine` (spec string, only for `kind: machine`).

- **Decision: JSON requirements feed `QuestDef` detection fields so existing
  triggers keep firing.**
  `QuestManager::checkCraftCompletion` / `checkInventory` / `checkBlockAction`
  / `checkToolCharged` / `checkSideConfigured` match on
  `QuestDef.detectType` + `QuestDef.detectTarget` (populated today by
  `QuestData::LoadCSV` from quests.csv). Moving requirements to JSON MUST NOT
  orphan these triggers: `QuestData` SHALL merge `kind` → `DetectionType`
  (`craft`→CRAFT, `obtain`→INVENTORY, `place`→BLOCK_PLACED, unknown → warn +
  render-only, no trigger) and `item` → `detectTarget` (already hierarchical
  spec string, byte-identical to what `checkCraftCompletion` compares via
  `ItemRegistry::idToHierarchical`). Detection behavior is unchanged; only the
  data source moves.

- **Decision: `auto_complete` gates instant completion.**
  Each detection handler (`checkCraftCompletion`, `checkInventory`, etc.)
  consults the quest's `auto_complete` flag before calling
  `completeQuestInternal`:
  - `auto_complete: true` (default) — current behavior: requirement met +
    prerequisites OK → complete immediately.
  - `auto_complete: false` — requirement met → transition to AVAILABLE (so the
    player sees "requirements done, press Complete"), no instant completion.
    Manual `completeQuest` (existing client button) then completes it.
  This is a pure client-state concern on the server; no wire change.

- **Decision: `machine`-kind detection on machine output.**
  A `machine` requirement (`{kind: machine, item, machine}`) completes when
  the player obtains `item` from a machine of type `machine`. Integration
  point: the machine output slot take path (`SetMachineSlotReq` /
  `player.machine.slot` handling in SimulationCore, where playerId + machine
  block id + item id are all available) — a new `checkMachineOutput(playerId,
  machineId, itemId, count)` on `QuestManager`, mirroring
  `checkCraftCompletion`. Falls back to `INVENTORY`-style detection for the
  item when the machine attribution is unavailable (item simply held).

- **Alternatives considered:**
  - *Keep rewards/requirements in CSV, widen columns* — cannot express N
    entries / choice / typed values without unbounded column repetition; CSV
    escapes (`"..."`) would fight the current naive `getline` parser in
    `QuestData.cpp:24`.
  - *Extend `quest_graph.json`* — conflates topology with content; the graph
    file is loaded by every quest consumer regardless of reward/requirement
    need.
  - *Single combined `quest_data.json`* — one file for both concerns; rejected
    for separation of concerns: MetaDB grant path and client render path
    differ, and combining would force both loaders to parse the union.

## Risks / Trade-offs

- **Two data paths during migration** → MetaDB and client must switch to JSON
  in the same change; until then rewards/requirements render/issue from JSON
  while CSV columns are dropped. Mitigation: single change, CSV columns
  emptied (not repurposed), `quests.csv` parser keeps accepting 12-field rows.
- **Triggers broken by data move** → if `QuestDef.detectType`/`detectTarget`
  stop being populated, `checkCraftCompletion` & co. silently match nothing.
  Mitigation: QuestData merge decision above + unit test asserting a CRAFT
  quest's detectType/detectTarget survive JSON load.
- **`auto_complete: false` quests stuck forever** → requirement met but
  quest stays AVAILABLE; player must click Complete. Mitigation: quest book
  shows a distinct "requirements done" state (progress 100% / green check) so
  the button is discoverable; `completeQuest` already validates
  AVAILABLE + `CanComplete`.
- **Machine attribution unreliable** → output-slot take path may not always
  carry machine block id. Mitigation: fall back to INVENTORY-style detection
  (item held ≥ count) when machine attribution is absent.
- **Schema drift between Go and C++ parsers** → both consume the same files;
  C++ side gets unit tests that load the shipped JSON and assert parse +
  item packing.
- **Choice rewards grant-all by mistake** → `rewards` XOR `choice_of`
  enforced by a validation pass in tests (both parsers).
- **Empty rewards/requirements for most quests** → content seeding is staged
  (base era first); UI renders no reward row when a quest has no definition
  (graceful, matches current behavior).

## Migration Plan

1. Create `data/quests/quest_rewards.json` + `data/quests/quest_requirements.json`
   with definitions for base-era quests (Vagrant) as the seed set; quests
   without rewards/requirements omitted.
2. quest_lib: add `QuestRewards::LoadJSON()` + `QuestRequirements::LoadJSON()`;
   client loads them beside `quests.csv`.
3. MetaDB: replace CSV reward/requirement lookup in `loadQuestDefinitions()`
   with the JSON loads; keep cost/cooldown in CSV.
4. QuestManager: gate `completeQuestInternal` behind `auto_complete`; add
   `checkMachineOutput` hooked to the machine output-slot take path.
5. Drop requirement/reward columns from `quests.csv` (keep
   `cost_item`/`cost_count`/`cooldown` for EXCHANGE).
6. UI renders requirement + reward icons from JSON; hide/show Complete button
   per `auto_complete`.
7. Rollback: restore CSV columns + revert loaders; JSON files harmless if
   unused.

## Resolved Decisions (from prior Open Questions)

- **Requirement kinds for the 163 quests.** Map existing CSV detect types to
  JSON `kind`s: 148 `craft` → `craft`, 10 `inventory` → `obtain`, 4
  `block_placed` → `place`, 1 `side_configured` → `side_configured` (renders
  only, no new trigger). The `machine` kind starts with seed data empty (no
  quest currently requires a machine-produced item); the detection path is
  implemented so content can exercise it later.
- **Choice-of-N supports ALL reward types.** `choice_of` is not restricted to
  `type: "item"` — an option may be any entry type (`item`/`experience`/`special`),
  and a mixed option list (e.g. pick item OR experience) is valid. The player
  picks one of the listed options. This change defines + renders the options;
  redemption UX remains a future change (Non-Goal).
- **`auto_complete` default: `true`** (preserves current instant-completion
  behavior). Content that wants a manual-complete quest seeds it explicitly
  with `"auto_complete": false` in `quest_requirements.json`. No quest is
  flipped to manual-complete in the initial seed — all 163 stay `true`.
