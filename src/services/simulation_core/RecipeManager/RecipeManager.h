#pragma once

// ---------------------------------------------------------------------------
// Forwarding header — delegates to the shared recipe_manager_lib
// and adds the ECS-specific evaluateConditions overload.
// ---------------------------------------------------------------------------

#include <recipe_manager_lib/RecipeManager.h>
#include <entt/entt.hpp>

namespace RecipeManager {

// ECS-specific overload: extracts machine state from ECS registry
// and delegates to evaluateConditions(const std::string&, const MachineState&)
bool evaluateConditions(const std::string& recipeId,
                        entt::registry& reg,
                        uint32_t x, uint32_t y, uint32_t z,
                        const ::RecipeManager::RecipeManager& mgr);

} // namespace RecipeManager
