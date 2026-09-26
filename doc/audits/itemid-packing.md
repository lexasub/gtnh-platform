# Audit: ItemId packing vs items.csv and machines.yaml (gp-4bh)

**Date**: 2026-09-26
**Scope**: `ItemId::pack` (`src/engine/registry/ItemId.h:85-131`) versus every row of
`src/content/data/registry/items.csv` and every `block_id` in
`src/content/data/registry/machines.yaml`, plus the consistency of the reject-and-warn rule
across all cell-parsing sites.
**Method**: `ItemId::pack` re-implemented line-for-line in Python (including the `plen > 15`
early return and the two digit-scanning loops) and run over all 414 data rows. Values cross-checked
against the C++ boundary constants computed the same way. No registry or data file was modified.

## Headline result

**The data files are clean.** 384 item rows and 30 machine variants: zero payload overflows, zero
duplicate packed values, zero rows with non-`0`/`1` prefix characters, zero collisions between
the two files. There is no data bug to fix here.

**But the reject-and-warn house rule is applied at only 2 of 4 cell-parsing sites**, and the
scheme's own documentation is wrong in a way that will produce a real data bug the next time
someone edits a row.

## Part 1 — The data files round-trip

### items.csv (384 data rows, 500 lines including comments)

```
rows: 384   payload-overflow rows: 0   duplicate packed values: 0
pack()==0 rows: 2  → line 1 (the "int" header) and line 7 ("0:0:0",air)
```

`pack()` returns 0 for exactly two rows and both are correct: the header line is skipped by
`ItemRegistry.cpp:45-47` before packing, and `0:0:0` is genuinely air. No legitimate row packs to 0.

No row has a prefix segment containing a character other than `0`, `1`, or `:`. That matters
because `pack` **silently skips** any other character (`ItemId.h:104-114` only advances `plen` on
`'0'`/`'1'` and has no `else`), so a typo like `1110:xx1:0` would pack as a *shorter* prefix and
land in the wrong category with no warning. No such row exists today.

No payload overflows its field width. For a prefix of length `plen` the payload must fit in
`16 - plen` bits; the largest payload found is `1110:010:48` (payload 48, `plen=9`, max 127) and
`1110:100:1` (payload 1, `plen=7`, max 511). Comfortable headroom everywhere.

### machines.yaml (30 `block_id` rows, 361 lines)

```
rows: 30   payload-overflow rows: 0
```

Every `block_id` is present verbatim as the first column of an `items.csv` row, **except four**.
26 of 30 match exactly, e.g. `machines.yaml:37` `1110:000:0` ↔ `items.csv:344` `1110:000:0`
(`heat_furnace`).

## Part 2 — DATA BUG: four `block_id: "0"` rows in machines.yaml

```
machines.yaml:235   block_id: "0"   name: placeholder_electrolyser
machines.yaml:255   block_id: "0"   name: placeholder_chemical_reactor
machines.yaml:265   block_id: "0"   name: placeholder_assembler
machines.yaml:275   block_id: "0"   name: placeholder_crystallizer
```

All four pack to `0x0000`, the same value as `items.csv:7` `0:0:0,air`. They are the only four
`block_id`s in the file with no matching `items.csv` text, and their names match no `items.csv`
row either.

This is a **data** defect, and per the brief it is the more expensive kind: it will survive every
code fix, and it is invisible by value.

Why it is not caught: `RecipeManager::parseYamlMachineClass` at
`src/game/recipes/RecipeManager.cpp:373` packs the cell and then **deliberately skips the zero
check**:

```cpp
mv.block_id = ItemId::pack(v["block_id"].as<std::string>(""));
...
if (mv.block_id > 0) {                       // :381
    if (classByBlockId_.count(mv.block_id) > 0) {
        spdlog::warn("Block ID {} already mapped to class '{}', overriding with '{}'", ...);
    }
    classByBlockId_[mv.block_id] = def.name;  // :386
    ...
}
```

`mv.block_id > 0` at `:381` means all four placeholders are **dropped from
`classByBlockId_`, `tierByBlockId_` and `energyInByBlockId_`** — they are silently ignored
rather than rejected with a warning. So today the practical effect is benign: no machine is
registered at block 0, and air cannot be mistaken for an electrolyser.

The risk is the *next* edit. Once these four machines are implemented, someone will replace
`"0"` with a real `1110:...` id. If they forget one, the row vanishes with no log line. And
`tools/todo_items/issue21.md:82-85` already assigns them ids that **contradict** the packing
scheme:

```
placeholder_electrolyser        0  57352  1110:00:8
placeholder_chemical_reactor    0  57353  1110:00:9
placeholder_assembler           0  57354  1110:00:10
placeholder_crystallizer        0  57355  1110:00:11
```

`1110:00:8` has `plen=6`, so `pack` puts the 6-bit prefix in the top 6 bits → `0xC000 | 8 =
0xC008` = **category ORES (49152-57343)**, not MACHINES. The correct 4-bit form is `1110:000:8`.
The TODO file's ids are one bit short and land the machine in the wrong category entirely. If
those values are ever copied into `machines.yaml`, four machines become invisible to every
`ItemId::category()`-gated path.

## Part 3 — CODE GAP: the house rule is applied at 2 of 4 parse sites

The rule: `pack` returns 0 for any cell with no digits, so a typo and a deliberate air are
identical **by value**. Accept 0 only for the literal texts `"0"` and `"0:0:0"`; otherwise reject
and warn. Checked all four sites:

| Site | file:line | Rule applied? | Behaviour on a bad cell |
|---|---|---|---|
| `ItemRegistry` (items.csv) | `ItemRegistry.cpp:56-60` | **yes** | `spdlog::warn`, `continue` |
| `ClientItemRegistry` (items.csv) | `ClientItemRegistry.cpp:40-43` | **yes** | `continue` (silent) |
| `BlockDrops` (drops.csv) | `BlockDrops.cpp:24-30` | **yes** | `spdlog::warn`, reject line |
| `RecipeManager::parseYamlMachineClass` (machines.yaml) | `RecipeManager.cpp:373,381` | **NO** | silently drops the variant via `if (mv.block_id > 0)` |

The BlockDrops rule was added today (commit `91819986`, "reject unknown drop ids"); the comment at
`BlockDrops.cpp:13-18` states the rule and names its siblings as precedent. `RecipeManager` is the
odd one out, and it is the one that reads a file with a known bad row.

Also inconsistent, lower severity: `ClientItemRegistry.cpp:41` and `:43` apply the air-literal
test and then immediately drop the row anyway (`if (id == 0) continue;`), so `air` is never
registered client-side. `ItemRegistry` does register it. That asymmetry is intentional per
`test_recipe_mirrors.cpp:344-347` ("`GetAllItemIds` does not include the skipped air id") but it
means the two loaders disagree about whether air exists — worth a comment, not a bug.

## Part 4 — Documentation bug in ItemId.h: the worked example is wrong

```
// Example: "0:10:3"  → prefix bits "010", payload 3 → 0x4003      ItemId.h:26
//          "1111:0:5" → prefix bits "11110", payload 5 → 0xF005    ItemId.h:27
```

Both are wrong.

- `"0:10:3"`: prefix bits are `0`+`1`+`0` = `010` (3 bits), so `shift = 16-3 = 13` and the
  result is `(0b010 << 13) | 3` = `0x4003`. The comment's *value* is right. But
  `unpack("0x4003")` returns `"0:16387"`, not `"0:10:3"` — `unpack` is lossy by design
  (`ItemId.h:137-147`, its own comment says "top level only"), so the example implies a
  round-trip that does not exist. The header's category table at `:19-24` documents a
  two-level scheme (`10` = ORES, payload 14 bits) while the example uses three segments, so
  which is normative is unclear from the file alone.
- `"1111:0:5"`: prefix bits are `1111`+`0` = `11110` (5 bits), `shift = 11`,
  `(0b11110 << 11) | 5` = `0xF005`. The comment's value is right; the same non-round-trip applies.

So the examples' *arithmetic* is correct; what is wrong is that the file reads as though
`unpack` recovers the hierarchical string. Given that `unpack` is used for display/debug only
(I found no production consumer — see below), this is low impact, but it is the kind of comment
that invites a future dev to write `CHECK_EQ(unpack(pack(s)), s)`, which would fail on nearly
every row.

## Part 5 — `unpack` has no production consumer

```
$ grep -rnw unpack --exclude-dir=cmake-build-debug --exclude-dir=cmake-build-release \
    --exclude-dir=worktrees --exclude-dir=.git --exclude-dir=bgfx src tools test
```
Referenced only inside `ItemId.h` itself (by `categoryName(uint16_t)` and its own definition).
No caller in `src/`, `tools/`, or any test. Since `unpack` is lossy, its lack of a consumer is
consistent — and it means the "round-trip" framing in gp-4bh's original description does not
describe a real invariant. The invariant that *does* hold, and that I verified across all 414 rows,
is: **pack is injective** (no two distinct texts produce the same value, per file or across the
two files), and **pack is stable under re-parsing its own output's category**.

## Ranked findings

1. **Four `block_id: "0"` rows in machines.yaml** (`:235,:255,:265,:275`) silently dropped by
   `RecipeManager.cpp:381` with no warning. Data bug, survives code fixes. Benign today
   (block 0 = air is never registered as a machine) but a trap for the next edit.
2. **`tools/todo_items/issue21.md:82-85` proposes ids that pack into the wrong category.**
   `1110:00:8` → `0xC008` = ORES, not MACHINES. Needs `1110:000:8`. If these land in
   machines.yaml, four machines become invisible to every `ItemId::category()`-gated path.
   This is the highest-consequence item in the audit because it is *scheduled* to be applied.
3. **House rule not applied at `RecipeManager.cpp:373`** — the one parse site that reads a file
   known to contain bad ids. Add the same `"0"`/`"0:0:0"` literal test the other three use, and
   warn on rejection instead of silently skipping via `if (mv.block_id > 0)`.
4. **`ItemId.h:26-27` examples imply a round-trip that `unpack` cannot perform** (`:137-147`).
   Reword, or state explicitly that `unpack` is lossy by design.
5. **`ClientItemRegistry` drops air while `ItemRegistry` keeps it** — intentional and test-pinned,
   but undocumented at the code site. Comment only.

Items 1-3 are code/data defects worth a bead. Items 4-5 are comment fixes.
