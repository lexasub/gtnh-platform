#pragma once
#include "ISystem.h"
#include "../PatternLibrary.h"
#include "../RecipeManager/RecipeManager.h"
#include <entt/entt.hpp>
#include <memory>
#include <unordered_map>

namespace simcore {

class IEventPublisher;
class PipeEnergyClient;
class CraftReservationClient;

struct MultiblockController;
struct MachineComponent;

class EBFSystem : public ISystem {
public:
    EBFSystem(entt::registry& reg,
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

    static constexpr int KANHAL_MAX_HEAT = 1800;
    static constexpr int NICHROME_MAX_HEAT = 2700;
    static constexpr int TUNGSTENSTEEL_MAX_HEAT = 4500;

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

    static constexpr int COIL_LAYER_1 = 1;
    static constexpr int COIL_LAYER_2 = 2;
    static constexpr int COIL_DX = 1;
    static constexpr int COIL_DZ = 1;

    void tickEBF(uint64_t ctrl_id, MultiblockController& ctrl);
    void tickOrchestratedStart(entt::entity entity,
                               const RecipeManager::Recipe& recipe,
                               int input_start, int input_end);
    int detectHeatTier(const MultiblockController& ctrl) const;
    int getCoilHeat(uint16_t block_id) const;
};

} // namespace simcore
