#include "BoilerSystem.h"
#include "HeatConstants.h"
#include <common/ItemId.h>
#include <spdlog/spdlog.h>
#include "../components/HeatIntakeComponent.h"
#include "../components/EnergyStorage.h"

namespace simcore {

BoilerSystem::BoilerSystem(entt::registry& reg,
                           std::shared_ptr<IEventPublisher> events,
                           std::shared_ptr<PipeEnergyClient> pipeClient)
    : reg_(reg), events_(events), pipeClient_(pipeClient)
{
}

void BoilerSystem::tick(float /*dt*/) {
    // ── Steam solid boiler: water + heat → STEAM ──────────────────────
    auto solidView = reg_.view<MachineComponent, InventoryContainer, EnergyStorage, HeatIntakeComponent>();
    for (auto ent : solidView) {
        auto& machine = solidView.get<MachineComponent>(ent);
        if (machine.machine_id != ItemId::pack("1110:01:0")) continue;
        auto& container = solidView.get<InventoryContainer>(ent);
        auto& energy = solidView.get<EnergyStorage>(ent);
        auto& heatIntake = solidView.get<HeatIntakeComponent>(ent);

        if (energy.isFull()) continue;
        if (heatIntake.heat_stored <= 0) continue;
        if (container.slots.empty() || container.slots[0].count == 0 || container.slots[0].item_id != ItemId::pack("0:11111:0")) continue;

        heatIntake.heat_stored -= std::min(HeatConstants::CONVERSION_RATE, heatIntake.heat_stored);

        container.slots[0].count--;
        if (container.slots[0].count == 0)
            container.slots[0].item_id = ItemId::pack("0:11111:3");

        int32_t accepted = energy.produceEnergy(std::min(energy.maxOutput, HeatConstants::CONVERSION_RATE));
        if (accepted > 0) {
            spdlog::debug("Boiler {} at entity {} produced {} STEAM",
                          machine.machine_id, static_cast<uint32_t>(ent), accepted);
            if (pipeClient_) {
                pipeClient_->publishNodeUpdate(
                    static_cast<uint64_t>(ent),
                    static_cast<int32_t>(machine.x), static_cast<int32_t>(machine.y), static_cast<int32_t>(machine.z),
                    energy.current, energy.capacity, energy.maxInput, energy.maxOutput,
                    energy.tier, static_cast<int32_t>(energy.type),
                    true, false);
            }
        }
        events_->publishBlockEntityUpdate(machine.x, machine.y, machine.z, machine.machine_id,
                                          {}, 0.0f, static_cast<uint32_t>(energy.current),
                                          energy.type, 0, -1, heatIntake.ratio());
    }

    // ── Steam heat boiler: STEAM → HEAT converter ─────────────────────
    auto heatView = reg_.view<MachineComponent, EnergyStorage>();
    for (auto ent : heatView) {
        auto& machine = heatView.get<MachineComponent>(ent);
        if (machine.machine_id != ItemId::pack("1110:01:1")) continue;
        auto& energy = heatView.get<EnergyStorage>(ent);

        if (energy.type != EnergyType::STEAM) continue;
        if (energy.isEmpty()) continue;

        int32_t toConsume = std::min(energy.maxOutput, HeatConstants::CONVERSION_RATE);
        int32_t consumed = energy.consumeEnergy(toConsume);
        if (consumed <= 0) continue;

        // Attach HeatIntakeComponent if missing (SimulationEngine only attaches for HEAT-type machines)
        auto& heatIntake = reg_.get_or_emplace<HeatIntakeComponent>(ent);
        int32_t space = heatIntake.heat_capacity - heatIntake.heat_stored;
        if (space <= 0) continue;
        int32_t added = std::min(consumed, space);
        heatIntake.heat_stored += added;

        spdlog::debug("Heat boiler {} at entity {} produced {} HEAT from {} STEAM",
                      machine.machine_id, static_cast<uint32_t>(ent), added, consumed);

        if (pipeClient_) {
            pipeClient_->publishNodeUpdate(
                static_cast<uint64_t>(ent),
                static_cast<int32_t>(machine.x), static_cast<int32_t>(machine.y), static_cast<int32_t>(machine.z),
                heatIntake.heat_stored, heatIntake.heat_capacity,
                0, static_cast<int32_t>(added),
                energy.tier, static_cast<int32_t>(EnergyType::HEAT),
                true, false);
        }
        events_->publishBlockEntityUpdate(machine.x, machine.y, machine.z, machine.machine_id,
                                          {}, 0.0f, static_cast<uint32_t>(heatIntake.heat_stored),
                                          EnergyType::HEAT, 0, -1, heatIntake.ratio());
    }
}

} // namespace simcore
