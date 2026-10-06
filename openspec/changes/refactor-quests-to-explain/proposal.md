# Change: Make quests explain, not just require

## Why

Every one of our 163 quests is `craft X` or `obtain X`. The description field is
one sentence: *"Craft oak planks from your logs."* There is not a single quest
that teaches a mechanic.

GTNH's quest book is roughly half explaining content. From `Tier1LV` and
`MultiblockGoals` alone: *Why One Coke Oven When You Can Have Ten?*, *You're
Gonna Hate This #1-6*, *The Water Dilemma*, *Something From Nothing*, *Don't
ask how this works*, *Steam is Never Enough*. None of those require an item.
They exist to make the player understand why a machine exists, which is the
entire reason a tech-progression game is worth playing instead of a spreadsheet.

Our quest book is a checklist that happens to be sorted. That is the gap.

## What Changes

- **New requirement kind `explain`** — a quest with no item requirement,
  completed manually, whose payload is prose. This is the GTNH tutorial quest.
- **New optional columns in `quests.csv`**: `hint` (one-line nudge shown before
  completion) and `lore` (extended prose, collapsed by default in the UI).
- **Descriptive rewriting of all existing 163 quests** — each MUST state why the
  thing matters, not only what to make. Existing quest ids are preserved so
  `quest_graph.json` edges and saved player progress (MetaDB) stay valid.
- Sub-quest support: one quest may carry a small ordered list of sub-steps that
  gate its completion in order. This is a deliberately shallow subset of GTNH's
  per-quest node graph — enough for *"get X, then get Y, then combine"* without
  importing BetterQuesting's full AND/OR graph.
- Quest sections gain an explicit `unlocks_hint`, so the quest book can tell a
  player what a section is for before they enter it.
- The `explain` kind and the prose columns get a C++ loader path plus tests.

## Impact

- Affected specs: `questbook`, `questbook-quest-data` (both modified)
- Affected code:
  - `src/content/data/quests/quests.csv` (new columns, rewritten descriptions)
  - `src/content/data/quests/quest_requirements.json` (new `explain` kind)
  - `src/game/quests/*` (loader, `QuestKind` enum, completion path)
  - `src/protocol/quest.fbs` (sub-steps and hint fields on the wire)
  - `src/apps/game_client/*` (quest book UI: hint line, lore panel, sub-step
    checklist, manual Complete button for `explain` quests)
- **Risk — the manual-completion path already exists but is thinly tested.**
  `openspec/specs/manual-completion` covers it and
  `questbook-quest-data` already specifies `auto_complete: false` →
  AVAILABLE. `explain` quests are that path at scale, so the existing
  requirement gets a new consumer.
- **Risk — protocol change.** New FlatBuffers fields are additive, but
  `quest.fbs` is a shared schema across services. A field-number collision or a
  client/server version skew breaks quest rendering silently.
- **Migration:** 163 descriptions get rewritten. Quest ids do NOT change, so
  `quest_graph.json` prerequisites and any saved completion state survive. New
  prose is authored by hand, not generated.

## Non-Goals

- No import of BetterQuesting's JSON format or its full per-quest node graph.
  Sub-steps are a bounded subset; the graph stays in `quest_graph.json`.
- No new era or section until the explaining content exists to fill it.
- No localisation / i18n plumbing — prose is English, as the current quests are.