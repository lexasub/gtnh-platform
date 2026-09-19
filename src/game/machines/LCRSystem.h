#pragma once
#include <engine/sim/ISystem.h>
#include <engine/sim/PatternLibrary.h>
#include "RecipeManager/RecipeManager.h"
#include <engine/sim/components/MultiblockController.h>
#include <entt/entt.hpp>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace simcore {

class IEventPublisher;
class PipeEnergyClient;
class CraftReservationClient;

class LCRSystem : public ISystem {
public:
    LCRSystem(entt::registry& reg,
              std::unordered_map<uint64_t, MultiblockController>& controllers,
              const PatternRegistry& patterns,
              std::shared_ptr<RecipeManager::RecipeManager> recipes,
              std::shared_ptr<IEventPublisher> events,
              std::shared_ptr<PipeEnergyClient> pipeClient,
              std::shared_ptr<CraftReservationClient> reservations = nullptr);

    void tick(float dt) override;
    bool onConsumeResponse(uint64_t node_id, int32_t consumed, int32_t remaining);

    // Commit a fully-accepted pending craft (4.3.2) — same contract as
    // MachineSystem::commitPendingCraft.
    void commitPendingCraft(entt::entity entity);

private:
    entt::registry& reg_;
    std::unordered_map<uint64_t, MultiblockController>& controllers_;
    const PatternRegistry& patterns_;
    std::shared_ptr<RecipeManager::RecipeManager> recipes_;
    std::shared_ptr<IEventPublisher> events_;
    std::shared_ptr<PipeEnergyClient> pipeClient_;
    std::shared_ptr<CraftReservationClient> reservations_;
    std::unordered_map<uint64_t, int32_t> pendingConsumes_;
    uint64_t totalTicks_ = 0;

    void tickLCR(uint64_t ctrl_id, MultiblockController& ctrl);
    void tickOrchestratedStart(entt::entity entity,
                               const RecipeManager::Recipe& recipe,
                               int input_start, int input_end);
};

} // namespace simcore
