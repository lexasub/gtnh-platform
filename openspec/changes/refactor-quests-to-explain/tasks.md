## 1. Research (parallel with 2)

- [ ] 1.1 Extract GTNH's explaining quests from the modpack repo
      (`config/betterquesting/DefaultQuests/Quests/`) into
      `doc/research/gtnh-explaining-quests.md`. Focus on the tutorial class:
      *You're Gonna Hate This*, *Why One Coke Oven*, *Something From Nothing*,
      *The Water Dilemma*, *Steam is Never Enough*, and the MultiblockGoals
      explanations
- [ ] 1.2 Extract their `properties.betterquesting.desc` prose verbatim as
      reference, and classify each as: mechanic-teaching, frustration-warning,
      or design-rationale
- [ ] 1.3 Map each to our content: which of our machines/systems it explains,
      and whether we have the content to explain at all

## 2. Data format

- [ ] 2.1 Add `explain` to the requirement-kind enum in the quest data model,
      alongside `craft` / `obtain` / `place` / `machine`
- [ ] 2.2 Add `hint` and `lore` columns to `quests.csv`. Both optional — a
      missing column value MUST NOT fail the loader
- [ ] 2.3 Define the sub-step structure in `quest_requirements.json`: an ordered
      list under a quest, each step carrying its own kind and target, with the
      quest completing only when all steps are satisfied in order
- [ ] 2.4 Write the loader for `explain` and sub-steps in `src/game/quests/`
- [ ] 2.5 Wire the `explain` completion path: manual Complete button, gated by
      prerequisites like any other quest

## 3. Protocol

- [ ] 3.1 Add `hint`, `lore` and sub-step fields to `quest.fbs` with NEW field
      numbers — never renumber existing ones
- [ ] 3.2 Regenerate `quest_generated.h` and confirm client and server agree
- [ ] 3.3 Verify a client built against the old schema still renders (missing
      field ⇒ empty string, not a parse failure)

## 4. UI

- [ ] 4.1 Render the `hint` line under the quest title, dimmed
- [ ] 4.2 Render `lore` as a collapsed paragraph, expandable
- [ ] 4.3 Render sub-steps as an ordered checklist that ticks as they complete
- [ ] 4.4 Render the manual Complete button only for quests that need it, so the
      163 existing auto-complete quests do not grow a dead button
- [ ] 4.5 Show the section's `unlocks_hint` at the section header

## 5. Content (the actual point)

- [ ] 5.1 Rewrite all 163 existing quest descriptions in explaining style. Each
      MUST answer "why would I want this", not just "make this". Preserve every
      quest id
- [ ] 5.2 Add `explain` quests for the mechanics we have and never taught:
      why a boiler needs a pressure rating, why ore washing exists, what a
      multiblock buys over a single block, why hatches exist at all, what a
      circuit tier actually changes
- [ ] 5.3 Add `explain` quests for the three machines from
      `add-chemistry-progression` (why distil, why sift, why solidify)
- [ ] 5.4 Add `explain` quests for the coil ladder and generation line, per
      those changes
- [ ] 5.5 Add sub-step quests where the chain is genuinely ordered — "get oil,
      then crack it, then polymerise" is one quest with three steps, not three
      flat quests

## 6. Verification

- [ ] 6.1 `ctest -R quest` — graph, manager, construct, types, lib, and the new
      loader tests
- [ ] 6.2 New test: an `explain` quest with satisfied prerequisites is AVAILABLE
      and completes only via the manual path
- [ ] 6.3 New test: sub-steps gate completion and enforce order
- [ ] 6.4 New test: missing `hint`/`lore` columns load without error
- [ ] 6.5 Test: an old-schema client payload parses against the new schema
- [ ] 6.6 Assert no quest id changed: snapshot the 163 ids before the rewrite and
      diff after