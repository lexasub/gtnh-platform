#include "ClientRecipeDB.h"

namespace Crafting {

bool MatchGrid(const std::array<ItemStack, 9>& grid, ItemStack* out_result) {
    for (const auto& recipe : kRecipes) {
        bool match = true;
        for (int i = 0; i < 9; ++i) {
            if (recipe.input_slots[i].item_id != grid[i].item_id) {
                match = false;
                break;
            }
        }
        if (match) {
            *out_result = recipe.output;
            return true;
        }
    }
    *out_result = ItemStack{};
    return false;
}

}  // namespace Crafting
