## Context

GTNH's questbook is ~3800 quests across 47 lines. Our questbook is 163. The gap
is not a quality problem, it is a scope decision that had never been written
down: the four sibling changes cover the GregTech-technique slice, and everything
else fell through implicitly.

This change exists because "implicitly out of scope" and "decided out of scope"
behave differently. An implicit omission gets re-litigated every time someone
compares against GTNH; a recorded decision does not.

## Goals / Non-Goals

- Goals: record which GTNH areas are in scope, which are declined, and which are
  pending; name the dependencies between them; answer the three open questions
  that were blocking any of it
- Non-Goals: propose the pillars themselves; size the incremental items;
  implement anything

## Decisions

- **Decline rather than defer combat, farming, space and magic.** These are
  parallel tech/content trees, not extensions of GregTech. Deferring them implies
  they will arrive; declining them keeps the quest book's expectations honest.
  Alternatives considered: keep them all as "deferred" (rejected — reintroduces
  the re-litigation problem this change exists to solve), drop them silently
  (rejected — that is the status quo this change replaces).

- **Keep AE2 in scope as a pillar.** It is the natural deep-inventory answer for
  a tech-progression game and does not require mobs or farming. It needs a
  design decision on the network model before it can be specced, which is why
  task 1.1 is a design spike rather than an implementation.

- **Put nuclear behind chemistry.** The fuel cycle is refining work
  (maceration → processing → enrichment → fuel rod), so it is downstream of the
  chemistry tree. Reactor multiblocks are a separate body of work on top. Both
  are recorded as dependencies rather than assumed.

- **Keep the incremental items undecided.** Glass tiers, multiblock boosters,
  exotic hatches, conduits and the silicon chain ride on systems that already
  exist. They are not pillars, so the pillars decision does not settle them, and
  committing them now would be guessing at sizes.

- **Mark bio fuels as dropped-by-dependency.** Its content source is the farming
  tree, which is declined. Listing it as in scope while depending on declined
  content would be a lie in the backlog itself.

## Risks / trade-offs

- **Declining magic and space narrows the project's identity** → recorded as an
  explicit reversal path in the spec delta, so changing the decision is possible
  but has to be deliberate.
- **AE2 is under-specified** → the risk is writing a spec before the network
  model is decided. Mitigated by making the design spike task 1.1 and gating
  tasks 1.2+ on it.

## Migration Plan

None. No data or code changes. Archiving this change does not alter any spec —
the delta records constraints on future changes.

## Open Questions

- Concrete scope of each pillar is undecided; each gets its own proposal.
- The six incremental items need sizing before they can be scheduled.