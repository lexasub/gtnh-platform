#include "BoilerSystem.h"
#include <common/ItemId.h>
#include <spdlog/spdlog.h>
 #include "../components/HeatIntakeComponent.h"
#include "../components/EnergyStorage.h"

namespace simcore {

namespace {
    inline bool isBoiler(uint16_t block_id) {
        return block_id == ItemId::pack("1110:01:0")  // steam_solid_boiler
            || block_id == ItemId::pack("1110:01:1"); // steam_heat_boiler
    }

    constexpr int32_t kConversionRate = 1;
}

BoilerSystem::BoilerSystem(entt::registry& reg,
                           std::shared_ptr<IEventPublisher> events,
                           std::shared_ptr<PipeEnergyClient> pipeClient)
    : reg_(reg), events_(events), pipeClient_(pipeClient)
{
}

void BoilerSystem::tick(float /*dt*/) {
    auto view = reg_.view<MachineComponent, InventoryContainer, EnergyStorage, HeatIntakeComponent>();

    for (auto ent : view) {
        auto& machine = view.get<MachineComponent>(ent);
        auto& container = view.get<InventoryContainer>(ent);
        auto& energy = view.get<EnergyStorage>(ent);
        auto& heatIntake = view.get<HeatIntakeComponent>(ent);

        if (!isBoiler(machine.machine_id)) continue;
        if (energy.isFull()) continue;

        // Check HeatIntakeComponent.heat_stored > 0
        if (heatIntake.heat_stored <= 0) continue;

        // Check inventory slot 0 has water bucket
        if (container.slots.empty() || container.slots[0].count == 0 || container.slots[0].item_id != ItemId::pack("0:11111:0")) continue;

        // Consume 1 from heat_stored
        heatIntake.heat_stored -= std::min(1, heatIntake.heat_stored);

        // Consume water bucket: slot[0].count--
        container.slots[0].count--;
        if (container.slots[0].count == 0) {
            container.slots[0].item_id = ItemId::pack("0:11111:3");  // empty_bucket
        }

        // Produce STEAM: int32_t accepted = energy.addEnergy(min(energy.maxOutput, 1))
        int32_t accepted = energy.addEnergy(std::min(energy.maxOutput, 1));

        // If accepted > 0, publish node update to PipeNetwork
        if (accepted > 0) {
            spdlog::debug("Boiler {} at entity {} produced {} STEAM",
                          machine.machine_id, static_cast<uint32_t>(ent), accepted);

            // Notify PipeNetwork of energy state change
            if (pipeClient_) {
                pipeClient_->publishNodeUpdate(
                    static_cast<uint64_t>(ent),          // node_id = ECS entity id
                    static_cast<int32_t>(machine.x),
                    static_cast<int32_t>(machine.y),
                    static_cast<int32_t>(machine.z),
                    energy.current,
                    energy.capacity,
                    energy.maxInput,
                    energy.maxOutput,
                    energy.tier,
                    static_cast<int32_t>(energy.type),
                    true,   // is_source (boiler produces steam)
                    false   // is_sink
                );
            }
        }

        // Publish block entity update
        events_->publishBlockEntityUpdate(
            machine.x, machine.y, machine.z,
            machine.machine_id,
            {},
            0.0f,
            static_cast<uint32_t>(energy.current),
            energy.type);
    }
}

} // namespace simcore
