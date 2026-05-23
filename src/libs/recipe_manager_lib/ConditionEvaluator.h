#pragma once

#include "RecipeConditions.h"
#include "RecipeTypes.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace RecipeManager {

// Machine state for condition evaluation
// Aggregates relevant state from SimulationCore ECS components
struct MachineState {
    float temperature;              // °C
    float purity;                   // 0.0–1.0
    uint32_t energy;                // current EU stored
    uint16_t biome_id;              // current biome
    uint8_t facing;                 // block facing (0-5)
    std::vector<uint32_t> network_ids;  // connected networks
    // Fluids/gas/plasma — as ItemStack items (no separate fluid field)
    std::vector<SpecialCondition> tags; // machine-specific tags
};

// Evaluates whether a recipe can run given current machine/world state
class ConditionEvaluator {
public:
    ConditionEvaluator() = default;
    
    // Main entry point: check if recipe conditions are satisfied
    bool evaluate(const Recipe& recipe, const MachineState& machineState);
    
private:
    bool checkEnvironment(const EnvironmentConditions& env, 
                         const MachineState& state) const;
    bool checkMachine(const MachineConditions& mach, 
                     const MachineState& state) const;
    bool checkSpecial(const std::vector<SpecialCondition>& recipeTags,
                     const std::vector<SpecialCondition>& machineTags) const;
};

} // namespace RecipeManager
