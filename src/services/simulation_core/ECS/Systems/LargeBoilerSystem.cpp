#include "LargeBoilerSystem.h"
#include "../components/MachineComponent.h"
#include "../components/EnergyStorage.h"
#include "../components/InventoryContainer.h"
#include "../components/RecipeProgress.h"
#include "../components/HeatIntakeComponent.h"
#include "../components/OverheatComponent.h"
#include "../components/Position.h"
#include "../components/Block.h"
#include "../components/MultiblockController.h"
#include "../Network/IEventPublisher.h"
#include "../Network/PipeEnergyClient.h"
#include "../Network/ItemClient.h"
#include <spdlog/spdlog.h>

namespace simcore {

LargeBoilerSystem::LargeBoilerSystem(entt::registry& reg,
                                     std::unordered_map<uint64_t, MultiblockController>& controllers,
                                     const PatternRegistry& patterns,
                                     std::shared_ptr<IEventPublisher> events,
                                     std::shared_ptr<PipeEnergyClient> pipeClient,
                                     std::shared_ptr<ItemClient> itemClient)
    : reg_(reg), controllers_(controllers), patterns_(patterns),
      events_(events), pipeClient_(pipeClient), itemClient_(itemClient)
{}

void LargeBoilerSystem::tick(float) {
    for (auto& [ctrl_id, ctrl] : controllers_) {
        if (ctrl_id == 0) continue;
        auto* pattern = patterns_.getPattern(2);
        if (!pattern) continue;
        tickBoiler(ctrl_id, ctrl);
    }
}

void LargeBoilerSystem::tickBoiler(uint64_t ctrl_id, MultiblockController& ctrl) {
    (void)ctrl_id;
    auto view = reg_.view<const Position, MachineComponent>();
    entt::entity entity = entt::null;
    for (auto e : view) {
        auto& pos = view.get<const Position>(e);
        if (pos.x == ctrl.x && pos.y == ctrl.y && pos.z == ctrl.z) {
            entity = e;
            break;
        }
    }
    if (entity == entt::null) return;

    auto& machine = reg_.get<MachineComponent>(entity);
    auto& container = reg_.get<InventoryContainer>(entity);
    auto& heatIntake = reg_.get_or_emplace<HeatIntakeComponent>(entity);

    int fuel_slot = -1;
    for (size_t i = 0; i < container.slots.size(); ++i) {
        if (container.slots[i].item_id == COAL_BLOCK_ID ||
            container.slots[i].item_id == CHARCOAL_BLOCK_ID) {
            if (container.slots[i].count > 0) {
                fuel_slot = static_cast<int>(i);
                break;
            }
        }
    }

    if (fuel_slot >= 0) {
        container.slots[fuel_slot].count--;
        if (container.slots[fuel_slot].count == 0) {
            container.slots[fuel_slot].item_id = 0;
            container.slots[fuel_slot].meta = 0;
        }

        heatIntake.heat_stored += BOILER_HEAT_PER_FUEL;
        if (heatIntake.heat_stored > heatIntake.heat_capacity) {
            heatIntake.heat_stored = heatIntake.heat_capacity;
        }

        auto* energy = reg_.try_get<EnergyStorage>(entity);
        if (energy) {
            int32_t steam_produced = energy->addEnergy(STEAM_PER_WATER);
            if (steam_produced > 0 && pipeClient_) {
                pipeClient_->publishNodeUpdate(
                    static_cast<uint64_t>(entity),
                    static_cast<int32_t>(machine.x),
                    static_cast<int32_t>(machine.y),
                    static_cast<int32_t>(machine.z),
                    energy->current,
                    energy->capacity,
                    energy->maxInput,
                    energy->maxOutput,
                    energy->tier,
                    static_cast<int32_t>(EnergyType::STEAM),
                    true,
                    false);
            }
        }

        if (reg_.all_of<OverheatComponent>(entity)) {
            reg_.remove<OverheatComponent>(entity);
        }
    } else {
        if (heatIntake.heat_stored > 0) {
            heatIntake.heat_stored -= std::min(5, heatIntake.heat_stored);
        }

        if (heatIntake.heat_stored > heatIntake.heat_capacity * 0.8f) {
            auto& oh = reg_.emplace_or_replace<OverheatComponent>(entity);
            oh.state = OverheatState::WARNING;
        }
    }

    events_->publishBlockEntityUpdate(
        static_cast<int32_t>(machine.x),
        static_cast<int32_t>(machine.y),
        static_cast<int32_t>(machine.z),
        machine.machine_id,
        {},
        0.0f,
        0,
        EnergyType::STEAM,
        0,
        -1,
        heatIntake.ratio());
}

} // namespace simcore
