#include "GeneratorSystem.h"
#include "../../common/ItemId.h"
#include "../../libs/machine_registry/MachineRegistry.h"
#include "../components/HeatIntakeComponent.h"
#include <cstring>
#include <spdlog/spdlog.h>

namespace simcore {

namespace {
    inline bool isGenerator(uint16_t block_id) {
        return block_id == ItemId::pack("1110:00:2") || block_id == ItemId::pack("1110:01:0");
    }
}

const std::unordered_map<uint16_t, int32_t>& GeneratorSystem::FuelValues() {
    static const std::unordered_map<uint16_t, int32_t> kFuel = {
        {ItemId::pack("0:11110:2"), 8000},   // coal
        {ItemId::pack("0:10:00:0"), 2000},   // oak_planks
        {ItemId::pack("0:11110:0"), 500},    // stick
    };
    return kFuel;
}

GeneratorSystem::GeneratorSystem(entt::registry& reg,
                                 std::shared_ptr<IEventPublisher> events,
                                 std::shared_ptr<PipeEnergyClient> pipeClient)
    : reg_(reg), events_(events), pipeClient_(pipeClient)
{
}

void GeneratorSystem::tick(float /*dt*/) {
    auto view = reg_.view<MachineComponent, InventoryContainer, EnergyStorage>();

    for (auto ent : view) {
        auto& machine = view.get<MachineComponent>(ent);
        auto& container = view.get<InventoryContainer>(ent);
        auto& energy = view.get<EnergyStorage>(ent);

        if (!isGenerator(machine.machine_id)) continue;
        spdlog::info("[GeneratorSystem] processing entity {} machine_id={} slots={} coal={} energy={}/{}",
                     static_cast<uint32_t>(ent), machine.machine_id,
                     container.slots.size(),
                     (!container.slots.empty() ? container.slots[0].item_id : 0),
                     energy.current, energy.capacity);
        if (energy.isFull()) continue;

        int32_t& remaining = burnEnergy_[ent];
        if (remaining <= 0) {
            for (auto& slot : container.slots) {
                if (slot.count == 0) continue;
                auto it = FuelValues().find(slot.item_id);
                if (it != FuelValues().end()) {
                    slot.count--;
                    remaining = it->second;
                    burnFuel_[ent] = slot.item_id;
                    break;
                }
            }
            if (remaining <= 0) continue;
        }

        int32_t produced = std::min(energy.maxOutput, remaining);
        int32_t accepted = energy.addEnergy(produced);
        remaining -= accepted;

        if (pipeClient_ && energy.type == EnergyType::HEAT) {
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
                true,   // is_source (generator produces)
                false   // is_sink
            );
        }

        // ELECTRICITY generators also publish node update for CableGraph registration
        if (pipeClient_ && energy.type == EnergyType::ELECTRICITY) {
            pipeClient_->publishNodeUpdate(
                static_cast<uint64_t>(ent),
                static_cast<int32_t>(machine.x),
                static_cast<int32_t>(machine.y),
                static_cast<int32_t>(machine.z),
                energy.current,
                energy.capacity,
                energy.maxInput,
                energy.maxOutput,
                energy.tier,
                static_cast<int32_t>(energy.type),
                true,
                false
            );
        }

        if (remaining <= 0) {
            burnEnergy_.erase(ent);
            burnFuel_.erase(ent);
        }

        float pct = 0.0f;
        {
            auto it = FuelValues().find(container.slots.empty() ? 0 : container.slots[0].item_id);
            if (it != FuelValues().end() && it->second > 0) {
                pct = 1.0f - static_cast<float>(remaining) / static_cast<float>(it->second);
            }
        }
        std::vector<uint8_t> invData(container.slots.size() * 5);
        {
            uint8_t* ptr = invData.data();
            for (const auto& s : container.slots) {
                std::memcpy(ptr, &s.item_id, sizeof(uint16_t)); ptr += sizeof(uint16_t);
                *ptr++ = s.count;
                std::memcpy(ptr, &s.meta, sizeof(uint16_t)); ptr += sizeof(uint16_t);
            }
        }
        int slotsIn = static_cast<int>(container.slot_count);
        if (auto* info = MachineRegistry::instance()->Get(machine.machine_id)) {
            slotsIn = info->slots_in;
        }
        float heatRatio = 0.0f;
        if (auto* hic = reg_.try_get<HeatIntakeComponent>(ent)) {
            heatRatio = hic->ratio();
        }
        events_->publishBlockEntityUpdate(
            machine.x, machine.y, machine.z,
            machine.machine_id,
            invData,
            pct,
            static_cast<uint32_t>(energy.current),
            energy.type,
            static_cast<uint32_t>(energy.capacity),
            slotsIn,
            heatRatio);
    }
}

} // namespace simcore
