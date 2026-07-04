#pragma once

#include <array>
#include <string>

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

// 13 base crafting_table recipes
constexpr Recipe kRecipes[] = {
    // base:crafting_table (planks)
    // slot mapping: 13=top-left, 32=stick
    {{{{13, 1}, {13, 1}, {}, {13, 1}, {13, 1}, {}, {}, {}, {}}}, {14, 1}},

    // base:crafting_table_cobblestone
    {{{{7, 1}, {7, 1}, {}, {7, 1}, {7, 1}, {}, {}, {}, {}}}, {14, 1}},

    // base:crafting_table_iron
    {{{{4, 1}, {4, 1}, {}, {4, 1}, {4, 1}, {}, {}, {}, {}}}, {14, 1}},

    // base:stick
    // pattern: two planks vertical on left column
    {{{{13, 1}, {}, {}, {13, 1}, {}, {}, {}, {}, {}}}, {32, 4}},

    // base:wooden_pickaxe
    // planks: top row (3), middle (2), right-middle (1)
    // sticks: left-middle (1), right-middle (1)
    {{{{13, 1}, {13, 1}, {13, 1}, {}, {32, 1}, {}, {}, {32, 1}, {}}}, {33, 1}},

    // base:stone_pickaxe
    {{{{7, 1}, {7, 1}, {7, 1}, {}, {32, 1}, {}, {}, {32, 1}, {}}}, {34, 1}},

    // base:iron_pickaxe
    {{{{4, 1}, {4, 1}, {4, 1}, {}, {32, 1}, {}, {}, {32, 1}, {}}}, {35, 1}},

    // base:furnace
    // planks: top row (3), bottom row (3), left-middle (1)
    // cobblestone: middle (1), right-middle (1)
    {{{{7, 1}, {7, 1}, {7, 1}, {7, 1}, {}, {7, 1}, {7, 1}, {7, 1}, {7, 1}}},
     {36, 1}},

    // base:chest
    // planks: top row (4), middle (3), right-bottom (1)
    {{{{13, 1},
       {13, 1},
       {13, 1},
       {13, 1},
       {},
       {13, 1},
       {13, 1},
       {13, 1},
       {13, 1}}},
     {37, 1}},

    // base:torch
    // coal/charcoal: top-left (1)
    // stick: middle (1)
    {{{{44, 1}, {}, {}, {32, 1}, {}, {}, {}, {}, {}}}, {40, 4}},

    // base:wooden_axe
    // planks: top-left, top-middle, middle-left, middle-right
    // sticks: left-middle, right-bottom
    {{{{13, 1}, {13, 1}, {}, {13, 1}, {32, 1}, {}, {}, {32, 1}, {}}}, {41, 1}},

    // base:wooden_shovel
    // plank: top-left
    // stick: middle (1)
    // wood: right-bottom (1)
    {{{{13, 1}, {}, {}, {32, 1}, {}, {}, {32, 1}, {}, {}}}, {42, 1}},

    // base:wooden_sword
    // plank: top-middle
    // sticks: middle-left, middle-right
    {{{{}, {13, 1}, {}, {}, {32, 1}, {}, {}, {32, 1}, {}}}, {43, 1}},
};

// Returns true if grid matches any recipe, and sets result to the output item
// Returns false if no match found
bool MatchGrid(const std::array<ItemStack, 9> &grid, ItemStack *out_result);

} // namespace Crafting
