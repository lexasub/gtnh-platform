#pragma once

#include "ItemId.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
  std::uint16_t fluid_id = 0;
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
  [[nodiscard]] const FluidDefinition* fluid(std::uint16_t fluid_id) const;
  [[nodiscard]] const FluidDefinition* fluidByItem(std::uint16_t item_id) const;
  [[nodiscard]] const std::vector<DropDefinition>& drops() const { return drops_; }

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
// Canonical packed item identity for the Steam fluid.  Keep resource call sites
// independent of the literal while the full registry loader is integrated into
// this service's startup path.
inline constexpr std::uint16_t steamItemId() noexcept {
  return ItemId::pack("1111:11:1");
}

} // namespace gtnh::common
