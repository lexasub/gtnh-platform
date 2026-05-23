#include "RecipeManager.h"
#include "ECS/components/MachineComponent.h"
#include "ECS/components/Position.h"
#include "ECS/components/Block.h"
#include "ECS/components/EnergyStorage.h"
#include "ECS/components/TemperatureComponent.h"
#include "ECS/components/PurityComponent.h"
#include "ECS/components/BiomeComponent.h"
#include "ECS/components/NetworkConnectionComponent.h"
#include "ECS/components/MachineTagComponent.h"

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
