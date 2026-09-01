#pragma once

#include "RecipeConditions.h"
#include "recipe_generated.h"
#include <common/ResourcePort.h>
#include <array>
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

// One externally supplied resource a recipe consumes (openspec
// refactor-fluid-port-accounting 4.1.1). Generic by design so STEAM and
// future electricity share the same orchestration contract:
//   kind        — transport channel (gtnh::common::ResourceKind, the domain
//                 enum from src/common/ResourcePort.h; services map it to the
//                 wire with ResourcePortClient.h ToWire/FromWire)
//   resource_id — packed canonical items.csv id for FLUID/ITEM, 0 for energy
//                 channels (EU/HU/RU carry no material id)
//   amount      — units charged per tick while the recipe runs (same
//                 per-tick semantics as Recipe::energy_cost)
//   tier        — minimum machine variant tier the requirement applies to
struct ResourceRequirement {
  gtnh::common::ResourceKind kind = gtnh::common::ResourceKind::FLUID;
  std::uint32_t resource_id = 0;
  std::uint32_t amount = 0;
  std::int16_t tier = 0;

  [[nodiscard]] bool isEnergy() const noexcept {
    return kind == gtnh::common::ResourceKind::EU ||
           kind == gtnh::common::ResourceKind::HU ||
           kind == gtnh::common::ResourceKind::RU;
  }
  [[nodiscard]] bool needsResourceId() const noexcept {
    return kind == gtnh::common::ResourceKind::FLUID ||
           kind == gtnh::common::ResourceKind::ITEM;
  }
  [[nodiscard]] bool valid() const noexcept {
    return amount > 0 && (!needsResourceId() || resource_id != 0);
  }
};

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
  uint8_t unlock_era = 0; // quest era required to see this recipe (0 = always)
  uint32_t duration;
  float energy_cost;   // energy consumed per tick (eu / recipe base)
  float energy_output; // energy produced per operation (0 for consumers)
  RecipeConditions conditions;

  // Explicit external resource requirements (4.1.1). Empty = the legacy
  // energy_in/eu path drives the machine (migration compatibility); non-empty
  // = SimulationCore reserves every entry through the typed consume-request
  // contract before inputs are consumed or progress starts.
  std::vector<ResourceRequirement> resource_requirements;

  // Optional positional 3x3 pattern (crafting table / workbench).
  // When set, `matches` compares the container positionally (index 0 =
  // top-left, 8 = bottom-right) and `craft` consumes per-slot. Empty cell =
  // item_id 0. This makes the server agree with the client's positional
  // preview instead of the aggregate `inputs` matching.
  bool has_pattern = false;
  std::array<ItemStack, 9> pattern{};

  bool matches(const std::vector<ItemStack> &container_items) const;

  /// True when the recipe declares explicit resource requirements and must be
  /// orchestrated through the reservation contract (4.1.4 execution contract).
  [[nodiscard]] bool hasResourceRequirements() const noexcept {
    return !resource_requirements.empty();
  }

  /// Total per-tick amount across all requirements (0 when none).
  [[nodiscard]] std::uint32_t resourceAmountPerTick() const noexcept {
    std::uint32_t total = 0;
    for (const auto &req : resource_requirements) {
      total += req.amount;
    }
    return total;
  }
  /// Consume the recipe inputs from the container (pattern or aggregate),
  /// WITHOUT placing outputs. Used by the workbench where the result goes to
  /// the result slot + player inventory, not into an input-grid slot.
  std::vector<ItemStack>
  consumeInputs(const std::vector<ItemStack> &container_items) const;
  /// Consume inputs AND place outputs into the container (machine crafting).
  std::vector<ItemStack>
  craft(const std::vector<ItemStack> &container_items) const;
};

} // namespace RecipeManager
