#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Common/Inventory.h"

namespace MachineRecipes {

struct MachineRecipe {
  std::string name; // recipe key from JSON (e.g. "base:macerator_iron_ore")
  std::vector<ItemStack> inputs;
  std::vector<ItemStack> outputs;
  uint32_t duration;

  MachineRecipe() = default;
  MachineRecipe(std::string_view n, std::vector<ItemStack> in,
                std::vector<ItemStack> out, uint32_t dur)
      : name(n), inputs(std::move(in)), outputs(std::move(out)), duration(dur) {
  }
};

uint16_t MachineTypeFromFilename(const std::string &name);

inline std::unordered_map<uint16_t, std::vector<MachineRecipe>> s_recipes;

void LoadAll();

void LoadFromDirectory(const std::string &dirPath);

inline const std::vector<MachineRecipe> &GetRecipes(uint16_t machineType) {
  return s_recipes[machineType];
}

} // namespace MachineRecipes
