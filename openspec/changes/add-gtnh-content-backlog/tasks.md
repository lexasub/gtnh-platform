# Change: GTNH content backlog — scope decision

## Why

The four sibling changes cover chemistry, coils, generation, and explaining
quests. The remaining GTNH areas were researched and split by the owner into
three in-scope pillars, four declined trees, and six small incremental items.
This change records the decision so it is not re-opened.

## What Changes

- No code, data, or spec changes. This is a decision record.
- Three areas are marked in scope as future changes: transport,
  AE2-style storage networks, nuclear.
- Four areas are marked **rejected**: mobs/combat, farming/biomes/bees, Space
  Race, Thaumaturgy. These are declined, not deferred.
- Glass tiers and exotic hatches are folded into `add-coil-upgrade-line`, since
  both ride on the blast-furnace ladder.
- Four incremental items (multiblock boosters, conduits, silicon chain, bio
  fuels) remain pending a size call. Bio fuels are marked effectively dropped
  because they depend on rejected farming content.
- The three open questions from the first draft are recorded as answered.
- Dependency relationships between in-scope areas and the sibling changes are
  recorded.

## Impact

- Affected specs: none directly; the delta records constraints
- Affected code: none
- Follow-up: each in-scope pillar becomes its own `openspec/changes/` proposal

## When to delete

This change is archived once the three in-scope pillars have proposals and the
remaining incremental items have been sized. It is a decision record, not a
permanent spec.