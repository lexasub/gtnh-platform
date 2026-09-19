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

bool evaluateConditions(const std::string& recipeId,
                        entt::registry& reg,
                        uint32_t x, uint32_t y, uint32_t z,
                        const ::RecipeManager::RecipeManager& mgr) {
    MachineState state{};

    auto view = reg.view<simcore::MachineComponent>();
    for (auto entity : view) {
        auto& mc = view.get<simcore::MachineComponent>(entity);
        if (mc.x != x || mc.y != y || mc.z != z) {
            continue;
        }
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
        break;
    }

    return mgr.evaluateConditions(recipeId, state);
}

} // namespace RecipeManager
