#include "ConditionEvaluator.h"
#include "RecipeManager.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace RecipeManager {

bool ConditionEvaluator::evaluate(const Recipe& recipe, 
                                 const MachineState& machineState) {
    // If recipe has no conditions, it's always satisfied
    if (!recipe.conditions.environment && 
        !recipe.conditions.machine && 
        recipe.conditions.special.empty()) {
        return true;
    }
    
    // Check environment conditions
    if (recipe.conditions.environment) {
        if (!checkEnvironment(*recipe.conditions.environment, machineState)) {
            return false;
        }
    }
    
    // Check machine conditions
    if (recipe.conditions.machine) {
        if (!checkMachine(*recipe.conditions.machine, machineState)) {
            return false;
        }
    }
    
    // Check special conditions
    if (!recipe.conditions.special.empty()) {
        if (!checkSpecial(recipe.conditions.special, 
                         machineState.tags)) {
            return false;
        }
    }
    
    return true;
}

bool ConditionEvaluator::checkEnvironment(const EnvironmentConditions& env,
                                         const MachineState& state) const {
    // Check temperature.
    //
    // A NaN temperature satisfies `t < min || t > max` because every comparison
    // against NaN is false, so a corrupt or uninitialised reading made the
    // machine unconditionally runnable (gp-paja). std::isnan rejects it
    // explicitly: a NaN reading is not evidence that the machine is in range,
    // so it is treated like an out-of-range one.
    if (env.temperature) {
        if (std::isnan(state.temperature) ||
            state.temperature < env.temperature->min ||
            state.temperature > env.temperature->max) {
            return false;
        }
    }

    // Check purity — same NaN hole as the temperature gate above.
    if (env.purity) {
        if (std::isnan(state.purity) || state.purity < *env.purity) {
            return false;
        }
    }
    
    // Check biomes
    if (!env.biomes.empty()) {
        if (std::find(env.biomes.begin(), env.biomes.end(), 
                     state.biome_id) == env.biomes.end()) {
            return false;
        }
    }
    
    return true;
}

bool ConditionEvaluator::checkMachine(const MachineConditions& mach,
                                     const MachineState& state) const {
    // Check energy range
    if (mach.energy_min) {
        if (state.energy < *mach.energy_min) {
            return false;
        }
    }
    if (mach.energy_max) {
        if (state.energy > *mach.energy_max) {
            return false;
        }
    }
    
    // Check network
    if (mach.network_id) {
        if (std::find(state.network_ids.begin(), state.network_ids.end(),
                     *mach.network_id) == state.network_ids.end()) {
            return false;
        }
    }
    
    // Check facing
    if (mach.facing) {
        if (state.facing != *mach.facing) {
            return false;
        }
    }
    
    return true;
}

bool ConditionEvaluator::checkSpecial(const std::vector<SpecialCondition>& recipeTags,
                                     const std::vector<SpecialCondition>& machineTags) const {
    // Build hash map from machine tags: key → tag (O(machineTags))
    std::unordered_map<uint16_t, const SpecialCondition*> machineTagMap;
    machineTagMap.reserve(machineTags.size());
    for (const auto& tag : machineTags) {
        machineTagMap[tag.key] = &tag;
    }

    // For each recipe tag, O(1) lookup in machine tags
    for (const auto& recipeTag : recipeTags) {
        auto it = machineTagMap.find(recipeTag.key);
        if (it == machineTagMap.end()) {
            return false; // key not found
        }

        const auto& machineTag = *it->second;

        // Value types must match
        if (recipeTag.value_type != machineTag.value_type) {
            return false;
        }

        // Values must match based on type
        switch (recipeTag.value_type) {
            case 0: // int32
                if (recipeTag.int_value != machineTag.int_value) {
                    return false;
                }
                break;
            case 1: // float
                // The tolerance test `abs(a - b) > 0.001f` is false when either
                // side is NaN (the difference is NaN, and NaN > 0.001f is
                // false), so NaN used to match ANY float tag (gp-paja). The
                // isnan guard turns "unmeasurable" into a non-match, matching
                // the behaviour infinity already had.
                if (std::isnan(recipeTag.float_value) ||
                    std::isnan(machineTag.float_value) ||
                    std::abs(recipeTag.float_value - machineTag.float_value) > 0.001f) {
                    return false;
                }
                break;
            case 2: // string
                if (recipeTag.string_value != machineTag.string_value) {
                    return false;
                }
                break;
            default:
                return false; // unknown type
        }
    }

    return true;
}

} // namespace RecipeManager