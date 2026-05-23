#pragma once
#include <vector>
#include <cstdint>
#include "RecipeManager/RecipeManager.h"
namespace simulation_core {
class CraftInventoryFlow {
public:
    CraftInventoryFlow();
    // Consume items from player inventory slots, add results
    // Returns true if consumption succeeded
    bool applyCraftResult(uint64_t playerId,
                         const std::vector<RecipeManager::ItemStack>& consumed,
                         const std::vector<RecipeManager::ItemStack>& results);
    // Get resulting inventory delta (consumed + added items)
    struct CraftDelta { std::vector<RecipeManager::ItemStack> netChange; };
    CraftDelta getCraftDelta() const;
};
}