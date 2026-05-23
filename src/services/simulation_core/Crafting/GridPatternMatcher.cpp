// GridPatternMatcher.cpp
#include "GridPatternMatcher.h"
#include "fmt/format.h"

namespace Crafting {

bool GridPatternMatcher::slotMatches(const RecipeManager::ItemStack& grid_slot,
                                    const GridSlot& pattern_slot,
                                    uint8_t& consumed) {
    // SHAPE MATTERS: if pattern slot has item_id=0, grid slot MUST be empty
    if (pattern_slot.item_id == 0) {
        if (grid_slot.item_id != 0) {
            consumed = 0;
            return false;
        }
        consumed = grid_slot.count;
        return true;
    }

    // Non-zero pattern item_id must match grid item_id
    if (grid_slot.item_id != pattern_slot.item_id) {
        consumed = 0;
        return false;
    }

    // Pattern count=0 means "any count", >0 means minimum required
    if (pattern_slot.count > 0 && grid_slot.count < pattern_slot.count) {
        consumed = 0;
        return false;
    }

    // Pattern metadata=0 means "any meta", >0 means exact match
    if (pattern_slot.metadata > 0 && grid_slot.metadata != pattern_slot.metadata) {
        consumed = 0;
        return false;
    }

    // Match: consume all items in this slot
    consumed = grid_slot.count;
    return true;
}

MatchResult GridPatternMatcher::matchDetailed(const std::vector<RecipeManager::ItemStack>& grid,
                                               const GridPattern& pattern) {
    MatchResult result;
    result.matched = false;
    result.consumed_per_slot.resize(9, 0);

    for (int i = 0; i < 9; ++i) {
        const auto& grid_slot = grid[i];
        const auto& pattern_slot = pattern.slots[i];
        uint8_t consumed = 0;

        if (slotMatches(grid_slot, pattern_slot, consumed)) {
            result.consumed_per_slot[i] = consumed;
        } else {
            result.consumed_per_slot[i] = 0;
            return result;  // Early exit on mismatch
        }
    }

    result.matched = true;
    return result;
}

bool GridPatternMatcher::matchSingle(const std::vector<RecipeManager::ItemStack>& grid,
                                     const GridPattern& pattern) {
    return matchDetailed(grid, pattern).matched;
}

std::string GridPatternMatcher::match(const std::vector<RecipeManager::ItemStack>& grid,
                                      const std::vector<std::pair<std::string, GridPattern>>& patterns) {
    for (const auto& [recipe_name, pattern] : patterns) {
        if (matchSingle(grid, pattern)) {
            return recipe_name;
        }
    }
    return "";
}

}  // namespace Crafting
