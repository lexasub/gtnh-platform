#pragma once

#include "RecipeConditions.h"
#include "recipe_generated.h"
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace RecipeManager {

// Lean item representation for inventories, containers, and wire protocol.
// No recipe-specific fields — those go in InputItem.
struct ItemStack {
  uint16_t item_id;
  uint8_t count;
  uint16_t metadata;
};

// Input item in a recipe: extends ItemStack with consume/replace semantics.
// Used only inside Recipe::inputs — never for inventory or wire format.
struct InputItem {
  uint16_t item_id;
  uint8_t count;
  uint16_t metadata;
  bool consume = true; // false = keep container (bucket etc.) after craft
  uint16_t replace_item =
      0; // when !consume: replace with this item_id (0 = no replace)
  uint16_t replace_meta = 0; // metadata of the replacement item
};

struct OutputItem {
  uint16_t item_id;
  uint8_t count;
  uint16_t metadata;
  std::optional<std::string> display_name;
  std::optional<nlohmann::json> nbt;
  std::optional<std::string> color;
  std::optional<std::vector<std::string>> lore;
  std::optional<std::string> unlocalized_name;
};

/// Sentinel: recipe matches any machine energy type
static constexpr uint8_t ENERGY_TYPE_ANY = 255;

struct Recipe {
  std::string id;
  std::vector<InputItem> inputs;
  std::vector<OutputItem> outputs;
  uint16_t machine_id = 0;   // legacy: block_id
  std::string machine_class; // new: class name (empty = legacy format)
  int16_t min_tier = 0;      // new: inclusive lower tier bound
  int16_t max_tier = 32767;  // new: inclusive upper tier bound (INT16_MAX)
  uint8_t energy_type =
      ENERGY_TYPE_ANY; // filter: machine must have this energy_in (255 = any)
  uint32_t duration;
  float energy_cost;   // energy consumed per tick (eu / recipe base)
  float energy_output; // energy produced per operation (0 for consumers)
  RecipeConditions conditions;

  bool matches(const std::vector<ItemStack> &container_items) const;
  std::vector<ItemStack>
  craft(const std::vector<ItemStack> &container_items) const;
};

} // namespace RecipeManager
