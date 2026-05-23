// GridPatternMatcher.h
#pragma once

#include <array>
#include <string>
#include <vector>
#include "RecipeManager/RecipeManager.h"

namespace Crafting {

struct GridSlot {
    uint16_t item_id;    // 0 = empty
    uint8_t count;       // 0 = any, >0 = minimum required
    uint16_t metadata;   // 0 = any, >0 = exact match
};

struct GridPattern {
    std::array<GridSlot, 9> slots;
};

struct MatchResult {
    bool matched;
    std::vector<uint8_t> consumed_per_slot;
};

class GridPatternMatcher {
public:
    // Match grid against all patterns, return recipe name if found
    std::string match(const std::vector<RecipeManager::ItemStack>& grid,
                      const std::vector<std::pair<std::string, GridPattern>>& patterns);

    // Match grid against single pattern
    bool matchSingle(const std::vector<RecipeManager::ItemStack>& grid,
                     const GridPattern& pattern);

    // Detailed match with consumption tracking
    MatchResult matchDetailed(const std::vector<RecipeManager::ItemStack>& grid,
                              const GridPattern& pattern);

private:
    // Check single slot against GridSlot
    bool slotMatches(const RecipeManager::ItemStack& grid_slot,
                     const GridSlot& pattern_slot,
                     uint8_t& consumed);
};

}  // namespace Crafting
