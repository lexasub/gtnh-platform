#pragma once
#include <cstdint>
#include <vector>

#include <game/recipes/RecipeConditions.h>

namespace simcore {

struct MachineTagComponent {
  std::vector<RecipeManager::SpecialCondition> tags;

  MachineTagComponent() = default;
  explicit MachineTagComponent(
      std::vector<RecipeManager::SpecialCondition> tags)
      : tags(std::move(tags)) {}
};

} // namespace simcore
