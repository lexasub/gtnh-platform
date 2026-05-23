#include "CraftInventoryFlow.h"
namespace simulation_core {
CraftInventoryFlow::CraftInventoryFlow() = default;
bool CraftInventoryFlow::applyCraftResult(uint64_t playerId,
                                         const std::vector<RecipeManager::ItemStack>& consumed,
                                         const std::vector<RecipeManager::ItemStack>& results) {
    (void)playerId;  // unused in this simplified impl
    netChange.reserve(consumed.size() + results.size());
    netChange.insert(netChange.end(), consumed.begin(), consumed.end());
    netChange.insert(netChange.end(), results.begin(), results.end());
    return true;
}
CraftInventoryFlow::CraftDelta CraftInventoryFlow::getCraftDelta() const {
    return CraftDelta{netChange};
}
}