#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace RecipeManager {

// Environmental conditions (world-related)
struct TemperatureRange {
  float min;
  float max;
};

struct EnvironmentConditions {
  std::optional<TemperatureRange> temperature; // °C
  std::optional<float> purity;                 // 0.0–1.0
  std::vector<uint16_t> biomes;                // biome IDs
};

// Machine/technical conditions (machine configuration)
struct MachineConditions {
  std::optional<uint32_t> energy_min; // min EU/t
  std::optional<uint32_t> energy_max; // max EU/t
  std::optional<uint32_t> network_id; // pipe/energy network ID
  std::optional<uint8_t> facing;      // block facing (0-5)
};

// Special condition = typed key-value tag (like NBT)
// value_type: 0=int32, 1=float, 2=string
struct SpecialCondition {
  uint16_t key;       // ID from registry
  uint8_t value_type; // 0=int32, 1=float, 2=string
  int32_t int_value;
  float float_value;
  std::string string_value;
};

// All conditions for a recipe
struct RecipeConditions {
  std::optional<EnvironmentConditions> environment;
  std::optional<MachineConditions> machine;
  std::vector<SpecialCondition> special;
};

} // namespace RecipeManager
