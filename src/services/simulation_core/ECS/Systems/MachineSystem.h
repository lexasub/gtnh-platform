#pragma once

#include <entt/entt.hpp>
#include <memory>
#include <unordered_map>
#include <spdlog/spdlog.h>
#include "ISystem.h"
#include "../../RecipeManager/RecipeManager.h"
#include "../../Network/IEventPublisher.h"
#include "../components/MachineComponent.h"
#include "../components/RecipeProgress.h"
#include "../components/InventoryContainer.h"
#include "../components/EnergyStorage.h"
#include "MachineRegistry.h"

namespace simcore {

class PipeEnergyClient;

class MachineSystem : public ISystem {
public:
    MachineSystem(entt::registry& reg,
                  std::shared_ptr<RecipeManager::RecipeManager> recipes,
                  std::shared_ptr<IEventPublisher> events,
                  std::shared_ptr<PipeEnergyClient> pipeClient);

    void tick(float dt) override;
    void onConsumeResponse(uint64_t node_id = 0, int32_t consumed = 0, int32_t remaining = 0);

    // Force a republish of all machine state every N ticks so reconnecting clients catch up
    static constexpr int kForcePublishInterval = 100;

private:
    entt::registry& reg_;
    std::shared_ptr<RecipeManager::RecipeManager> recipes_;
    std::shared_ptr<IEventPublisher> events_;
    std::shared_ptr<PipeEnergyClient> pipeClient_;
    std::unordered_map<uint64_t, int32_t> pendingConsumes_;
    std::unordered_map<uint64_t, uint64_t> lastInventoryHash_;
    int tickCounter_ = 0;
};

} // namespace simcore
