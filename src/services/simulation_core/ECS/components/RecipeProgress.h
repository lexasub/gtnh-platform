#pragma once
#include <cstdint>
#include <string>

namespace simcore {

struct RecipeProgress {
    std::string recipe_id;  // current recipe ID, empty string if idle
    uint32_t remaining_ticks = 0;   // ticks left until completion
    bool is_processing = false;     // true while recipe is active
    bool needs_output = false;      // true when recipe complete, output pending

    // Default constructor: all zeros, empty recipe_id
    RecipeProgress() = default;
};

} // namespace simcore
