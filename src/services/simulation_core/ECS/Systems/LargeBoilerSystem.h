#pragma once
#include "ISystem.h"
#include "../PatternLibrary.h"
#include <entt/entt.hpp>
#include <memory>
#include <unordered_map>

namespace simcore {

class IEventPublisher;
class PipeEnergyClient;
class ItemClient;

struct MultiblockController;

class LargeBoilerSystem : public ISystem {
public:
    LargeBoilerSystem(entt::registry& reg,
                      std::unordered_map<uint64_t, MultiblockController>& controllers,
                      const PatternRegistry& patterns,
                      std::shared_ptr<IEventPublisher> events,
                      std::shared_ptr<PipeEnergyClient> pipeClient,
                      std::shared_ptr<ItemClient> itemClient);

    void tick(float dt) override;

    static constexpr uint16_t COAL_BLOCK_ID = 1010;
    static constexpr uint16_t CHARCOAL_BLOCK_ID = 1011;
    static constexpr int BOILER_HEAT_PER_FUEL = 100;
    static constexpr int STEAM_PER_WATER = 10;
    static constexpr int WATER_PER_TICK = 1;

private:
    entt::registry& reg_;
    std::unordered_map<uint64_t, MultiblockController>& controllers_;
    const PatternRegistry& patterns_;
    std::shared_ptr<IEventPublisher> events_;
    std::shared_ptr<PipeEnergyClient> pipeClient_;
    std::shared_ptr<ItemClient> itemClient_;

    void tickBoiler(uint64_t ctrl_id, MultiblockController& ctrl);
};

} // namespace simcore
