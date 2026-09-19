#pragma once

#include "ItemId.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Canonical registry contract (openspec refactor-fluid-port-accounting,
// design decision §1): items.csv is the sole catalog and ID namespace for
// every item, block, pipe, cable, and fluid.  pipes.csv, cables.csv,
// fluids.csv, and drops.csv carry properties only and reference canonical
// packed item ids; there is no separate fluid_id, pipe_id, or cable_id type.
namespace gtnh::common {

struct ItemDefinition {
  std::uint16_t id;
  std::string id_text;
  std::string name;
  std::uint16_t stack_size = 64;
  std::uint16_t meta = 0;
};

struct PipeDefinition {
  std::uint16_t item_id;
  std::string name;
  int tier = 0;
  int flow_rate = 0;
  int items_per_sec = 0;
};

struct CableDefinition {
  std::uint16_t item_id;
  std::string name;
  int tier = 0;
  double loss = 0.0;
  int ampacity = 0;
  int insulation = 0;
};

struct FluidDefinition {
  std::uint16_t item_id = 0;
  std::string name;
  std::string color;
  double density = 0.0;
  bool gaseous = false;
  int temperature = 0;
};

struct DropDefinition {
  std::uint16_t source = 0;
  std::uint16_t result = 0;
  int count = 1;
  int meta = 0;
};

class Registry {
public:
  // Loads all CSVs and validates the complete set before replacing the data.
  // A failed load leaves the registry empty and provides every failure in
  // errors(), making startup validation deterministic and strict.
  bool load(std::string_view directory);
  bool load(std::string_view items, std::string_view pipes,
            std::string_view cables, std::string_view fluids,
            std::string_view drops);

  [[nodiscard]] bool valid() const { return errors_.empty() && !items_.empty(); }
  [[nodiscard]] const std::vector<std::string>& errors() const { return errors_; }

  [[nodiscard]] const ItemDefinition* item(std::uint16_t id) const;
  [[nodiscard]] const ItemDefinition* itemByName(std::string_view name) const;
  [[nodiscard]] const PipeDefinition* pipe(std::uint16_t id) const;
  [[nodiscard]] const CableDefinition* cable(std::uint16_t id) const;
  // Fluid properties are keyed by the canonical items.csv item id of the
  // fluid item; there is no separate fluid id namespace.
  [[nodiscard]] const FluidDefinition* fluid(std::uint16_t item_id) const;
  [[nodiscard]] const std::vector<DropDefinition>& drops() const { return drops_; }

  // Canonical Steam identity: the items.csv row named "steam", returned only
  // when fluids.csv maps a property row to that item id.  Zero when the
  // registry is not loaded or the fluids.csv mapping is missing.
  [[nodiscard]] std::uint16_t steamItemId() const;
  [[nodiscard]] const std::unordered_map<std::uint16_t, ItemDefinition>& items() const {
    return items_;
  }
  [[nodiscard]] const std::unordered_map<std::uint16_t, PipeDefinition>& pipes() const {
    return pipes_;
  }
  [[nodiscard]] const std::unordered_map<std::uint16_t, CableDefinition>& cables() const {
    return cables_;
  }
  // Keyed by canonical items.csv item id.
  [[nodiscard]] const std::unordered_map<std::uint16_t, FluidDefinition>& fluids() const {
    return fluids_;
  }

  // Strict parser used by callers that need to validate a registry reference.
  static bool parseItemId(std::string_view text, std::uint16_t& id);

private:
  void clear();
  void error(std::size_t line, std::string_view file, std::string_view message);
  bool loadItems(std::string_view path);
  bool loadPipes(std::string_view path);
  bool loadCables(std::string_view path);
  bool loadFluids(std::string_view path);
  bool loadDrops(std::string_view path);
  bool validateReferences();

  std::unordered_map<std::uint16_t, ItemDefinition> items_;
  std::unordered_map<std::string, std::uint16_t> names_;
  std::unordered_map<std::uint16_t, PipeDefinition> pipes_;
  std::unordered_map<std::uint16_t, CableDefinition> cables_;
  std::unordered_map<std::uint16_t, FluidDefinition> fluids_;
  std::vector<DropDefinition> drops_;
  std::vector<std::string> errors_;
};
// Pinned Steam identity for callers that do not hold a loaded Registry:
// the recipe-validation cross-check in RecipeManager, tests, and the
// FluidRegistry fallback. registry_test keeps this constant equal to
// Registry::steamItemId() resolved from items.csv + fluids.csv. Systems that
// advertise or drain steam must resolve the id once via Registry::steamItemId()
// and carry it, never call this constant per site.
inline constexpr std::uint16_t steamItemId() noexcept {
  return ItemId::pack("1111:11:1");
}

} // namespace gtnh::common
