#pragma once

#include <array>
#include <string>

#include <common/ItemId.h>
#include "Common/Inventory.h"

// ── ClientRecipeDB
// ───────────────────────────────────────────────────────────── Embedded
// crafting recipes for client-side preview. Each Recipe entry: { {slot0, slot1,
// ..., slot8}, {output_item_id, output_count} } slot = 0 means empty; item_id =
// 0 means empty slot
namespace Crafting {

struct Recipe {
  std::array<ItemStack, 9> input_slots;
  ItemStack output;
};

// 13 base crafting_table recipes — item ids are packed hierarchical (from items.csv).
constexpr Recipe kRecipes[] = {
    // base:crafting_table (oak_planks)
    {{{{ItemId::pack("0:10:00:0"), 1}, {ItemId::pack("0:10:00:0"), 1}, {},
       {ItemId::pack("0:10:00:0"), 1}, {ItemId::pack("0:10:00:0"), 1}, {},
       {}, {}, {}}},
     {ItemId::pack("0:10:11:1"), 1}},

    // base:crafting_table_cobblestone
    {{{{ItemId::pack("0:0:2"), 1}, {ItemId::pack("0:0:2"), 1}, {},
       {ItemId::pack("0:0:2"), 1}, {ItemId::pack("0:0:2"), 1}, {},
       {}, {}, {}}},
     {ItemId::pack("0:10:11:1"), 1}},

    // base:crafting_table_iron
    {{{{ItemId::pack("0:110:1"), 1}, {ItemId::pack("0:110:1"), 1}, {},
       {ItemId::pack("0:110:1"), 1}, {ItemId::pack("0:110:1"), 1}, {},
       {}, {}, {}}},
     {ItemId::pack("0:10:11:1"), 1}},

    // base:stick — two oak_planks vertical
    {{{{ItemId::pack("0:10:00:0"), 1}, {}, {},
       {ItemId::pack("0:10:00:0"), 1}, {}, {},
       {}, {}, {}}},
     {ItemId::pack("0:11110:0"), 4}},

    // base:wooden_pickaxe — 3 planks top, 2 sticks below
    {{{{ItemId::pack("0:10:00:0"), 1}, {ItemId::pack("0:10:00:0"), 1},
       {ItemId::pack("0:10:00:0"), 1},
       {}, {ItemId::pack("0:11110:0"), 1}, {},
       {}, {ItemId::pack("0:11110:0"), 1}, {}}},
     {ItemId::pack("0:11110:3"), 1}},

    // base:stone_pickaxe — 3 cobblestone top, 2 sticks below
    {{{{ItemId::pack("0:0:2"), 1}, {ItemId::pack("0:0:2"), 1},
       {ItemId::pack("0:0:2"), 1},
       {}, {ItemId::pack("0:11110:0"), 1}, {},
       {}, {ItemId::pack("0:11110:0"), 1}, {}}},
     {ItemId::pack("0:11110:4"), 1}},

    // base:iron_pickaxe — 3 iron ingots top, 2 sticks below
    {{{{ItemId::pack("0:110:1"), 1}, {ItemId::pack("0:110:1"), 1},
       {ItemId::pack("0:110:1"), 1},
       {}, {ItemId::pack("0:11110:0"), 1}, {},
       {}, {ItemId::pack("0:11110:0"), 1}, {}}},
     {ItemId::pack("0:11110:5"), 1}},

    // base:furnace — 8 cobblestone ring (empty center)
    {{{{ItemId::pack("0:0:2"), 1}, {ItemId::pack("0:0:2"), 1},
       {ItemId::pack("0:0:2"), 1},
       {ItemId::pack("0:0:2"), 1}, {}, {ItemId::pack("0:0:2"), 1},
       {ItemId::pack("0:0:2"), 1}, {ItemId::pack("0:0:2"), 1},
       {ItemId::pack("0:0:2"), 1}}},
     {ItemId::pack("1110:00:0"), 1}},

    // base:chest — 8 oak_planks ring (empty center)
    {{{{ItemId::pack("0:10:00:0"), 1},
       {ItemId::pack("0:10:00:0"), 1},
       {ItemId::pack("0:10:00:0"), 1},
       {ItemId::pack("0:10:00:0"), 1},
       {},
       {ItemId::pack("0:10:00:0"), 1},
       {ItemId::pack("0:10:00:0"), 1},
       {ItemId::pack("0:10:00:0"), 1},
       {ItemId::pack("0:10:00:0"), 1}}},
     {ItemId::pack("0:10:11:0"), 1}},

    // base:torch — coal on top, stick below
    {{{{ItemId::pack("0:11110:2"), 1}, {}, {},
       {ItemId::pack("0:11110:0"), 1}, {}, {},
       {}, {}, {}}},
     {ItemId::pack("0:11110:1"), 4}},

    // base:wooden_axe — 3 planks (top-left, top-middle, middle-left), 2 sticks
    {{{{ItemId::pack("0:10:00:0"), 1}, {ItemId::pack("0:10:00:0"), 1}, {},
       {ItemId::pack("0:10:00:0"), 1}, {ItemId::pack("0:11110:0"), 1}, {},
       {}, {ItemId::pack("0:11110:0"), 1}, {}}},
     {ItemId::pack("0:11110:6"), 1}},

    // base:wooden_shovel — 1 plank top-left, 2 sticks mid+right-bottom
    {{{{ItemId::pack("0:10:00:0"), 1}, {}, {},
       {ItemId::pack("0:11110:0"), 1}, {}, {},
       {ItemId::pack("0:11110:0"), 1}, {}, {}}},
     {ItemId::pack("0:11110:7"), 1}},

    // base:wooden_sword — 1 plank top-middle, 2 sticks mid
    {{{{}, {ItemId::pack("0:10:00:0"), 1}, {},
       {}, {ItemId::pack("0:11110:0"), 1}, {},
       {}, {ItemId::pack("0:11110:0"), 1}, {}}},
     {ItemId::pack("0:11110:8"), 1}},
};

// Returns true if grid matches any recipe, and sets result to the output item
// Returns false if no match found
bool MatchGrid(const std::array<ItemStack, 9> &grid, ItemStack *out_result);

} // namespace Crafting
