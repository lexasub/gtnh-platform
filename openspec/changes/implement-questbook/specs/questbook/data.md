# Quest Data Format Specification

**File**: `openspec/changes/implement-questbook/specs/questbook/data.md`
**Purpose**: Define format, conventions, and guidelines for quest content in `data/quests/`. Reference for agents creating or editing quest data.

---

## 1. CSV Format (`data/quests/quests.csv`)

### Columns

| # | Column | Type | Required | Description |
|---|--------|------|----------|-------------|
| 0 | `id` | uint32 | Yes | Unique quest ID. Sequential, no gaps recommended. Range: 1-65535. |
| 1 | `title` | string | Yes | Short quest name. Imperative mood. Max 60 chars. |
| 2 | `description` | string | Yes | Quest objective + lore. Technical explanation of what to do and why. |
| 3 | `era` | enum | Yes | One of: `vagrant`, `apprentice`, `expert`, `administrator` |
| 4 | `section` | string | Yes | Section within era. Snake_case. Max 30 chars. |
| 5 | `prereqs` | string | No | Semicolon-separated quest IDs. Empty = root quest. Format: `"1"` or `"1;4;7"` |
| 6 | `detect_type` | enum | Yes | Detection method: `craft`, `block_placed`, `tool_charged`, `side_configured` |
| 7 | `detect_target` | string | Yes | Item/block ID string matching the detection type (see §3) |
| 8 | `reward_item` | uint16 | Yes | Item ID to reward on completion. 0 = no item reward. |
| 9 | `reward_count` | uint8 | Yes | Quantity of reward item. |

### CSV Conventions

- Header row required: `id,title,description,era,section,prereqs,detect_type,detect_target,reward_item,reward_count`
- No quoting around fields (plain CSV). Commas inside description prohibited.
- Empty prereqs = empty cell (no space)
- UTF-8 encoding
- No trailing whitespace

### Era Values (must match `quest::Era` enum)

| CSV value | Era enum | Display label |
|-----------|----------|---------------|
| `vagrant` | `Era::VAGRANT` (0) | Vagrant |
| `apprentice` | `Era::APPRENTICE` (1) | Apprentice |
| `expert` | `Era::EXPERT` (2) | Expert |
| `administrator` | `Era::ADMINISTRATOR` (3) | Administrator |

### Detection Type Values (must match `quest::DetectionType` enum)

| CSV value | Enum | Description |
|-----------|------|-------------|
| `craft` | `DetectionType::CRAFT` (0) | Craft specific item |
| `block_placed` | `DetectionType::BLOCK_PLACED` (1) | Place specific block |
| `tool_charged` | `DetectionType::TOOL_CHARGED` (2) | Fully charge a tool |
| `side_configured` | `DetectionType::SIDE_CONFIGURED` (3) | Configure machine side |

---

## 2. JSON Graph Format (`data/quests/quest_graph.json`)

### Structure

```json
{
  "quests": [
    {
      "id": <uint32>,
      "prereqs": [<uint32>, ...],
      "position": {
        "x": <int>,
        "y": <int>
      }
    }
  ]
}
```

### Fields

- `id` — Quest ID, MUST match `id` in `quests.csv`
- `prereqs` — Array of prerequisite quest IDs. Empty array `[]` for root quests.
- `position` — Visual layout hint for graph rendering (optional, for future graph view). Not parsed at runtime.

### Rules

1. Every quest ID in `quests.csv` MUST have a corresponding entry in `quest_graph.json`
2. The `prereqs` array MUST be a superset of the `prereqs` column in CSV (specifically for `LoadGraph()`)
3. No cycles in the DAG
4. IDs must be unique across the file

---

## 3. Detection Target Reference

The `detect_target` column contains an item/block identifier. The format depends on `detect_type`:

### `craft` — Item ID from `data/registry/items.csv`

`detect_target` = the **sequential row index** (0-based) from `data/registry/items.csv`, skipping the header row.

**Example mappings** (from current `items.csv`):

| Item | items.csv row | detect_target value | Used in quest |
|------|---------------|---------------------|---------------|
| stick | 42 (row 43 - 1 header) | 42 | — |
| wooden_pickaxe | 45 | 45 (but quest says 33) | Quest 1 |
| stone_pickaxe | 46 | 46 | — |
| iron_pickaxe | 47 | 47 (but quest says 35) | Quest 3 |
| crafting_table | 20 | 20 (but quest says 14) | Quest 4 |
| furnace | 21 (→ heat_furnace) | 21 (but quest says 36) | Quest 5 |

> ⚠️ **Current quests.csv uses item IDs that do NOT match sequential items.csv indices.** This means either: (a) item IDs come from a different numbering scheme (e.g., `items.db` binary), or (b) quest CSV needs alignment. When implementing `GetQuestDefinition()` in Go, verify the actual item ID scheme used by the game engine and update quests.csv accordingly.

### `block_placed` — Block ID

Format: numeric block ID matching the game's block registry.

**Current blocks with meta-layer support**: stone (1), cobblestone (2), dirt (7), machines (1110:00:0-8, 1110:01:0-3, 1110:10:0-3, 1110:11:0-1)

### `tool_charged` — Tool item ID

Same item ID scheme as `craft`. Detects when `DrillSystem` or `ChainsawSystem` reports full charge for that tool.

**Tools with charge tracking**: drill_ulv (item 74), drill_lv (75), drill_mv (76), drill_hv (77), chainsaw_lv (78)

### `side_configured` — Machine block ID

Detects when `WrenchHandler` configures a side on a specific machine block.

---

## 4. Era and Section Design Guidelines

### Era Mapping

Each era represents a technology tier. Quest density should scale with complexity:

| Era | Theme | Quest count | Sections |
|-----|-------|-------------|----------|
| Vagrant | Stone Age → hand tools | 5-8 | getting_started, basic_crafting |
| Apprentice | Steam power → basic infrastructure | 10-12 | steam_power, infrastructure |
| Expert | Electric tools → automation | 10-14 | electric_tools, automation |
| Administrator | End-game → extreme voltage | 6-10 | advanced_tech |

### Section Naming Conventions

- `snake_case` — lowercase with underscores
- Max 30 chars
- Describe the mechanical theme, not specific items
- Examples: `getting_started`, `steam_power`, `electric_tools`, `automation`, `advanced_tech`, `logistics`, `infrastructure`, `processing`

### Quest Title Conventions

- Imperative mood: "Craft a furnace", "Wire up your base"
- Max 60 chars
- Include the key item/block name
- Avoid quest numbers in title

### Description Style

- 1-2 sentences
- First sentence: concrete objective ("Craft X and place it")
- Second sentence (optional): why this matters ("X doubles your ore output")
- Technical accuracy: use actual item/block names from registry

---

## 5. DAG Design Principles

### Linear Core Chain

Every era should have a linear backbone that gates major milestones:

```
1 → 2 → 3          (Vagrant core: stone → iron tools)
4 → 5              (Vagrant parallel: bench → furnace)
7 → 8 → 9→10→11   (Apprentice core: bronze → steam machines)
18 → 19 → 28 → 30 (Expert→Admin core: drill progression)
```

### Branching for Optional Paths

Secondary sections branch off the core chain:

```
8 (Steam Boiler)
├── 9  (Steam Macerator)    — core progression
├── 10 (Steam Compressor)   — optional parallel
├── 11 (Steam Extractor)    — optional parallel
├── 13 (Plumbing)           — infrastructure
├── 14 (Logistics)          — infrastructure
└── 16 (Getting Wired)      — era gate to Expert
```

### Era Gate Requirements

The transition between eras should be gated by specific quests:

| Gate quest | From | To | Purpose |
|-----------|------|----|---------|
| Quest 16: "Getting Wired" | Apprentice | Expert | Tin cable + generator unlocks all electric content |
| Quest 28: "MV Drilling" | Expert | Administrator | MV drill unlocks end-game tech |

### Graph Layout Convention for `position`

```
x: 0 = main vertical spine (core progression)
x: negative = left branches (steam/heat/processing)
x: positive = right branches (infrastructure/logistics/cables)
y: increases downward (chronological/tech order)
```

---

## 6. Reward Guidelines

### Reward Item Selection

- Reward items should be useful for the next step in progression, not duplicates of the crafted item
- Common pattern: reward with raw materials needed for the next quest
- Item ID 0 + count 0 = no reward (for simple introductory quests)

### Current Reward Patterns

| Pattern | Example | When to use |
|---------|---------|-------------|
| Raw material | bronze_ingot (53) x8 | Quest 7: "Bronze Age" — gives materials for next machines |
| Same item | chest (37) x1 | Quest 6: "Getting Organized" — gives you the thing you made (useful for stacks) |
| Next tier item | furnace (36/heat_furnace) x2, steam_macerator (51) x4 | Quest 5: "Furnace Mastery" — gives extra machines |
| Cable/pipe | cable_tin (66) x8 | Quest 16: "Getting Wired" — gives wiring materials |
| Tool | drill_ulv (74) x1 | Quest 18: gives you the tool to start next section |

### Reward Balancing

- **Common items** (ingots, plates): 4-8 as reward
- **Machines**: 1-4 as reward (player needs multiple for automation)
- **Tools**: exactly 1 (single tool per player)
- **Cables/pipes**: 4-8 (consumables for infrastructure building)

---

## 7. Example Entry — Complete Walkthrough

### Quest 8: "Steam Boiler"

**CSV row:**
```
8,Steam Boiler,Craft a steam solid boiler. Boilers burn solid fuel to produce steam for your machines.,apprentice,steam_power,7,craft,49,54,4
```

**Breakdown:**
- `id=8` — unique
- `title="Steam Boiler"` — short, imperative, names the key item
- `description="Craft a steam solid boiler..."` — mechanical explanation
- `era=apprentice` — second era
- `section=steam_power` — core section of apprentice era
- `prereqs=7` — requires "Bronze Age" (quest 7) first
- `detect_type=craft` — auto-detect on craft
- `detect_target=49` — item ID for steam_solid_boiler
- `reward_item=54` — reward: steam_macerator (next step in progression)
- `reward_count=4` — give 4 macerators

**JSON graph entry:**
```json
{ "id": 8, "prereqs": [7], "position": { "x": 0, "y": 4 } }
```

### Quest 18: "ULV Drilling"

**CSV row:**
```
18,ULV Drilling,Craft an ULV drill. Manual mining is obsolete — let the drill do the work.,expert,electric_tools,16,craft,74,74,1
```

**Breakdown:**
- `detect_type=craft`, `detect_target=74` = drill_ulv
- `reward_item=74` = same drill (you get one for free)
- This is the **era gate** — unlocks Expert era content

---

## 8. Validation Rules (for data editors)

When editing quest data, verify:

1. **No duplicate IDs** — every quest_id unique across `quests.csv` and `quest_graph.json`
2. **No missing IDs** — every ID in `quest_graph.json` has a corresponding row in `quests.csv`
3. **No cycles** — `prereqs` DAG must be acyclic
4. **Era consistency** — quests within same section should be in same era
5. **Reachable roots** — every quest must be reachable from at least one root quest (prereqs=empty)
6. **detect_target validity** — target item/block ID must exist in the relevant registry
7. **Reward plausibility** — reward_item should reference an existing item ID
8. **Section grouping** — each section should have 3-8 quests for balanced UI panels
