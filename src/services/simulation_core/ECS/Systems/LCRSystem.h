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

class LCRSystem : public ISystem {
public:
    LCRSystem(entt::registry& reg,
              std::unordered_map<uint64_t, MultiblockController>& controllers,
              const PatternRegistry& patterns,
              std::shared_ptr<RecipeManager::RecipeManager> recipes,
              std::shared_ptr<IEventPublisher> events,
              std::shared_ptr<PipeEnergyClient> pipeClient);

    void tick(float dt) override;

private:
    entt::registry& reg_;
    std::unordered_map<uint64_t, MultiblockController>& controllers_;
    const PatternRegistry& patterns_;
    std::shared_ptr<RecipeManager::RecipeManager> recipes_;
    std::shared_ptr<IEventPublisher> events_;
    std::shared_ptr<PipeEnergyClient> pipeClient_;

    void tickLCR(uint64_t ctrl_id, MultiblockController& ctrl);
};

} // namespace simcore
