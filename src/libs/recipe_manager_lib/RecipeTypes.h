#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "recipe_generated.h"
#include "RecipeConditions.h"

namespace RecipeManager {

// Lean item representation for inventories, containers, and wire protocol.
// No recipe-specific fields — those go in InputItem.
struct ItemStack {
    uint16_t item_id;
    uint8_t  count;
    uint16_t metadata;
};

// Input item in a recipe: extends ItemStack with consume/replace semantics.
// Used only inside Recipe::inputs — never for inventory or wire format.
struct InputItem {
    uint16_t item_id;
    uint8_t  count;
    uint16_t metadata;
    bool     consume = true;        // false = keep container (bucket etc.) after craft
    uint16_t replace_item = 0;      // when !consume: replace with this item_id (0 = no replace)
    uint16_t replace_meta = 0;      // metadata of the replacement item
};

struct OutputItem {
    uint16_t item_id;
    uint8_t  count;
    uint16_t metadata;
    std::optional<std::string> display_name;
    std::optional<nlohmann::json> nbt;
    std::optional<std::string> color;
    std::optional<std::vector<std::string>> lore;
    std::optional<std::string> unlocalized_name;
};

struct Recipe {
    std::string id;
    std::vector<InputItem> inputs;
    std::vector<OutputItem> outputs;
 uint16_t machine_id = 0;
    uint32_t duration;
    float energy_cost;
    RecipeConditions conditions;

    bool matches(const std::vector<ItemStack>& container_items) const;
    std::vector<ItemStack> craft(const std::vector<ItemStack>& container_items) const;
};

} // namespace RecipeManager
