#include "ConditionEvaluator.h"
#include "RecipeManager.h"
#include <algorithm>
#include <cmath>

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
    // Check temperature
    if (env.temperature) {
        if (state.temperature < env.temperature->min || 
            state.temperature > env.temperature->max) {
            return false;
        }
    }
    
    // Check purity
    if (env.purity) {
        if (state.purity < *env.purity) {
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
    // For each recipe tag, check if machine has matching tag with compatible value
    for (const auto& recipeTag : recipeTags) {
        bool foundMatch = false;
        
        for (const auto& machineTag : machineTags) {
            // Keys must match
            if (recipeTag.key != machineTag.key) {
                continue;
            }
            
            // Value types must match
            if (recipeTag.value_type != machineTag.value_type) {
                continue;
            }
            
            // Values must match based on type
            switch (recipeTag.value_type) {
                case 0: // int32
                    if (recipeTag.int_value != machineTag.int_value) {
                        continue;
                    }
                    break;
                case 1: // float
                    if (std::abs(recipeTag.float_value - machineTag.float_value) > 0.001f) {
                        continue;
                    }
                    break;
                case 2: // string
                    if (recipeTag.string_value != machineTag.string_value) {
                        continue;
                    }
                    break;
                default:
                    continue; // unknown type
            }
            
            foundMatch = true;
            break;
        }
        
        if (!foundMatch) {
            return false;
        }
    }
    
    return true;
}

} // namespace RecipeManager