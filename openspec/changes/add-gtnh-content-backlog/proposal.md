# Change: GTNH content backlog — scope decision

## Why

Research against the real GTNH questbook (extracted from
`config/betterquesting/DefaultQuests/Quests/` in the modpack repo) surfaced
more gaps than fit one batch. The four sibling changes cover chemistry, coils,
generation, and explaining quests.

The remaining areas split into two kinds that need different decisions:
**gameplay pillars** (transport, AE2-style networks, nuclear) and
**survival/adjacent trees** (mobs, farming, magic, space). The project owner has
decided which pillars are in scope. This change records that decision so it is
not re-litigated, and so the rejected half is a stated decision rather than an
omission.

## Scope decision

### In scope — future changes

| Area | GTNH source | Needs | Blocked on |
|---|---|---|---|
| Transport / getting around | `GettingAround` (64 quests) | vehicles, minecarts/rails, mounts | nothing — can start now |
| AE2-style storage networks | `SFMandComputers` | network storage, item/fluid transfer across distance, scanning | nothing — but needs a design decision on the network model |
| Nuclear | `PowerfulNuclearPhysics`, nuclear half of `HowtoGeneratePower` (136) | fuel cycle (U/Th/Pu/Naquadah → Naquadria), reactor multiblocks, reprocessing | `add-chemistry-progression` for the refining chain; reactor multiblock work |

Each becomes its own `openspec/changes/` proposal. None is bundled into the
four sibling changes — bundling a new pillar into a content change hides the
fact that a pillar is new.

### Rejected — out of scope

| Area | GTNH source | Reason |
|---|---|---|
| Mobs / combat | `KillAlltheThings` (83) | no combat loop; a pillar we are not building |
| Farming / biomes / bees | `TheGreenRevolution`, `ForestryandMulti`, `FlowerPower`, `HowtoBee` (188), `BeeBreeding` | no crop growth or pollination; `tree-generation` covers only trees |
| Space Race | `SpaceRace` | no rockets, no second dimension |
| Thaumaturgy | `NoviceThaumaturgy`, `AdeptThaumaturgy`, `FocusonWandFoci` | GregTech is the only tech tree |

These are not deferred — they are declined. A quest explaining combat, farming,
spaceflight or magic SHALL NOT be added, and no proposal should be opened for
them without an explicit reversal recorded here.

### Folded into `add-coil-upgrade-line`

These ride on the blast-furnace ladder and are implemented as part of it:

| Area | Why folded in |
|---|---|
| Glass tiers | the same coil ladder seen from the other side — each tier is smelted at the heat ceiling a coil unlocks |
| Exotic hatches | hatch variants on the same multiblock part slots the ladder already occupies, sharing the same find-hatches path |

### Small incremental items — pending a size call

These ride on systems that already exist but are not part of the ladder. They
are not the subject of the pillars decision above, so they are kept by default
but not yet committed:

| Area | GTNH source | Size | Notes |
|---|---|---|---|
| Multiblock boosters | `MultiblockGoals` — parallel / time / struct / meta upgrades | medium | needs per-multiblock upgrade slots; a distinct mechanism, not more parts |
| Conduit / pipe tiers | `Tier2MV` — conduits, dark steel pipes, conduit probe | medium | `pipes-cables-transport` is single-tier today |
| Silicon / wafer chain | `Tier2MV` — SiCl₄, trichlorosilane, wafer, boule | small | already named as chemistry follow-on work |
| Bio fuels | `Tier2MV` — bio chaff, plant mass, plantballs, fish oil, ethanol | — | **depends on rejected farming content**, so effectively dropped |

## Dependency graph

```
add-chemistry-progression ──► chemistry rides on fluid_solidifier/distillation
        │
        ├──► add-coil-upgrade-line ──► glass tiers, exotic hatches (folded in)
        │                           └─► multiblock boosters (still pending)
        │
        └──► add-generator-quest-line ──► nuclear (also needs reactor multiblocks)

transport          — no dependencies, can start in parallel
AE2 networks       — no dependencies, needs a design decision first
nuclear            — needs chemistry + reactor multiblocks
conduits / pipes   — needs its own proposal against pipes-cables-transport
```

## Resolved questions

These were open when this change was first drafted. They are now answered:

- **Does the project grow survival pillars?** Yes, for transport and AE2-style
  logistics. No, for combat and farming.
- **Is Space Race a goal?** No.
- **Is magic in scope?** No. GregTech is the only tree.

## How to use this file

Pick a row, copy it into a `proposal.md` under `## Why`, cite the GTNH quest
line as the scope source, and write its own spec delta. Do not batch rows — the
three in-scope pillars are independent of each other and of the four sibling
changes.

## When to delete this change

Archived once the three in-scope pillars have proposals and the incremental
items above have been sized. It is a decision record, not a permanent spec.