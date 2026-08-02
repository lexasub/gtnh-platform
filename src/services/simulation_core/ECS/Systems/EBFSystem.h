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

struct MultiblockController;

class EBFSystem : public ISystem {
public:
    EBFSystem(entt::registry& reg,
              std::unordered_map<uint64_t, MultiblockController>& controllers,
              const PatternRegistry& patterns,
              std::shared_ptr<RecipeManager::RecipeManager> recipes,
              std::shared_ptr<IEventPublisher> events,
              std::shared_ptr<PipeEnergyClient> pipeClient);

    void tick(float dt) override;

    static constexpr uint16_t KANHAL_COIL_BLOCK_ID = 1002;
    static constexpr uint16_t NICHROME_COIL_BLOCK_ID = 1007;
    static constexpr uint16_t TUNGSTENSTEEL_COIL_BLOCK_ID = 1008;
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

    static constexpr int COIL_LAYER_1 = 1;
    static constexpr int COIL_LAYER_2 = 2;
    static constexpr int COIL_DX = 1;
    static constexpr int COIL_DZ = 1;

    void tickEBF(uint64_t ctrl_id, MultiblockController& ctrl);
    int detectHeatTier(const MultiblockController& ctrl) const;
    int getCoilHeat(uint16_t block_id) const;
};

} // namespace simcore
