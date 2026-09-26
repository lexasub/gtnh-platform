#pragma once

// ---------------------------------------------------------------------------
// Forwarding header — delegates to the shared recipe_manager_lib
// and adds the ECS-specific evaluateConditions overload.
// ---------------------------------------------------------------------------

#include <entt/entt.hpp>
#include <game/recipes/RecipeManager.h>

namespace RecipeManager {

// ECS-specific overload: extracts the state of the ONE machine entity given and
// delegates to evaluateConditions(const std::string&, const MachineState&).
//
// This is the correct form whenever the caller knows which machine it is acting
// on: the state that gates a recipe start is that machine's OWN energy, purity,
// temperature, biome, networks and tags. MachineSystem holds the handle
// (gp-iv20).
//
// `entity` must still be a live machine; it is not checked against the caller's
// coordinates, and a null/other entity simply contributes the default state.
bool evaluateConditions(const std::string &recipeId, entt::registry &reg,
                        entt::entity entity,
                        const ::RecipeManager::RecipeManager &mgr);

// Legacy positional form, kept only for callers that genuinely have no entity
// handle. It scans view<MachineComponent> and uses the FIRST machine it finds at
// (x, y, z), so when several machines share those coordinates the answer depends
// on EnTT's iteration order — one co-located machine then decides for all of
// them (gp-iv20). Prefer the entity overload wherever a handle exists.
bool evaluateConditions(const std::string &recipeId, entt::registry &reg,
                        uint32_t x, uint32_t y, uint32_t z,
                        const ::RecipeManager::RecipeManager &mgr);

} // namespace RecipeManager
