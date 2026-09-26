#include "RecipeManager.h"
#include <engine/sim/components/MachineComponent.h>
#include <engine/sim/components/Position.h>
#include <engine/sim/components/Block.h>
#include <engine/sim/components/EnergyStorage.h>
#include <game/machines/TemperatureComponent.h>
#include <game/machines/PurityComponent.h>
#include <engine/sim/components/BiomeComponent.h>
#include <game/machines/NetworkConnectionComponent.h>
#include <game/machines/MachineTagComponent.h>

namespace RecipeManager {

namespace {

// Copy the condition-relevant state of ONE machine entity out of the registry.
// Every component is optional, so a machine missing any of them contributes
// that field's default — the same shape MachineState{} has.
MachineState stateOf(entt::registry& reg, entt::entity entity) {
    MachineState state{};
    if (auto* energy = reg.try_get<simcore::EnergyStorage>(entity)) {
        state.energy = static_cast<uint32_t>(energy->current);
    }
    if (auto* block = reg.try_get<simcore::Block>(entity)) {
        state.facing = block->meta;
    }
    if (auto* temp = reg.try_get<simcore::TemperatureComponent>(entity)) {
        state.temperature = temp->temperature;
    }
    if (auto* purity = reg.try_get<simcore::PurityComponent>(entity)) {
        state.purity = purity->purity;
    }
    if (auto* biome = reg.try_get<simcore::BiomeComponent>(entity)) {
        state.biome_id = biome->biome_id;
    }
    if (auto* net = reg.try_get<simcore::NetworkConnectionComponent>(entity)) {
        state.network_ids = net->network_ids;
    }
    if (auto* tags = reg.try_get<simcore::MachineTagComponent>(entity)) {
        state.tags = tags->tags;
    }
    return state;
}

} // namespace

bool evaluateConditions(const std::string& recipeId,
                        entt::registry& reg,
                        entt::entity entity,
                        const ::RecipeManager::RecipeManager& mgr) {
    // The state comes from the machine being acted on, not from a positional
    // search. Before gp-iv20 this scanned view<MachineComponent> and broke on
    // the FIRST position match, so a co-located machine's energy, purity,
    // temperature, biome, networks and tags decided whether a different machine
    // at the same (x,y,z) was allowed to start.
    return mgr.evaluateConditions(recipeId, stateOf(reg, entity));
}

bool evaluateConditions(const std::string& recipeId,
                        entt::registry& reg,
                        uint32_t x, uint32_t y, uint32_t z,
                        const ::RecipeManager::RecipeManager& mgr) {
    // Legacy positional path — see the header. Kept for callers without a
    // handle; the first-match scan is the defect gp-iv20 describes, so any new
    // caller must prefer the entity overload.
    auto view = reg.view<simcore::MachineComponent>();
    for (auto entity : view) {
        auto& mc = view.get<simcore::MachineComponent>(entity);
        if (mc.x != x || mc.y != y || mc.z != z) {
            continue;
        }
        return mgr.evaluateConditions(recipeId, stateOf(reg, entity));
    }

    return mgr.evaluateConditions(recipeId, MachineState{});
}

} // namespace RecipeManager
