#include "HeatTransferSystem.h"
#include "../components/Block.h"
#include <spdlog/spdlog.h>

namespace simcore {

HeatTransferSystem::HeatTransferSystem(entt::registry& reg,
                                       MachineRegistry& machineRegistry,
                                       std::shared_ptr<IEventPublisher> events)
    : reg_(reg), machineRegistry_(machineRegistry), events_(std::move(events))
{
}

void HeatTransferSystem::tick(float /*dt*/) {
    auto view = reg_.view<MachineComponent, EnergyStorage, Position>();

    // ═══════════════════════════════════════════════════════════════════
    // Pass 1: Heat transfer (adjacent producer → consumer)
    // ═══════════════════════════════════════════════════════════════════

    struct HeatProducer {
        entt::entity entity;
        uint32_t x, y, z;
        EnergyStorage* energy;
    };
    std::vector<HeatProducer> producers;
    for (auto ent : view) {
        auto& mc = view.get<MachineComponent>(ent);
        auto& energy = view.get<EnergyStorage>(ent);
        auto& pos = view.get<Position>(ent);
        if (energy.type != EnergyType::HEAT) continue;
        auto* info = machineRegistry_.Get(mc.machine_id);
        if (!info || info->role != MachineRole::PRODUCER) continue;
        if (energy.current <= 0) continue;
        producers.push_back({ent, pos.x, pos.y, pos.z, &energy});
    }

    static const int dirs[6][3] = {
        {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
    };

    if (!producers.empty()) {
        for (auto ent : view) {
            auto& mc = view.get<MachineComponent>(ent);
            auto& energy = view.get<EnergyStorage>(ent);
            auto& pos = view.get<Position>(ent);
            if (energy.type != EnergyType::HEAT) continue;
            auto* info = machineRegistry_.Get(mc.machine_id);
            if (!info || info->role != MachineRole::CONSUMER) continue;
            if (energy.current >= energy.capacity) continue;

            int32_t needed = energy.capacity - energy.current;
            if (needed <= 0) continue;

            for (auto& d : dirs) {
                int32_t nx = static_cast<int32_t>(pos.x) + d[0];
                int32_t ny = static_cast<int32_t>(pos.y) + d[1];
                int32_t nz = static_cast<int32_t>(pos.z) + d[2];
                if (nx < 0 || ny < 0 || nz < 0) continue;

                for (auto& prod : producers) {
                    if (prod.entity == ent) continue;
                    if (static_cast<int32_t>(prod.x) != nx ||
                        static_cast<int32_t>(prod.y) != ny ||
                        static_cast<int32_t>(prod.z) != nz) continue;
                    if (prod.energy->current <= 0) continue;

                    int32_t available = prod.energy->current;
                    int32_t transfer = std::min(needed, available);
                    if (transfer <= 0) continue;

                    prod.energy->current -= transfer;
                    energy.current += transfer;
                    needed -= transfer;

                    // Sync HeatIntakeComponent
                    if (auto* hic = reg_.try_get<HeatIntakeComponent>(ent)) {
                        hic->heat_stored = energy.current;
                    }

                    spdlog::debug("[HeatTransfer] {} → {} transferred {} heat ({},{},{})",
                                 mc.machine_id, static_cast<uint32_t>(ent),
                                 transfer, pos.x, pos.y, pos.z);

                    if (needed <= 0) break;
                }
                if (needed <= 0) break;
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // Pass 2: Overheat detection
    // ═══════════════════════════════════════════════════════════════════
    {
        auto oh_view = reg_.view<HeatIntakeComponent>();
        for (auto ent : oh_view) {
            auto& hic = oh_view.get<HeatIntakeComponent>(ent);
            float r = hic.ratio();

            if (r >= HeatConstants::OVERHEAT_CRITICAL_THRESHOLD) {
                auto& oh = reg_.emplace_or_replace<OverheatComponent>(ent, OverheatState::CRITICAL, 0);
                (void)oh;
            } else if (r >= HeatConstants::OVERHEAT_WARNING_THRESHOLD) {
                reg_.emplace_or_replace<OverheatComponent>(ent, OverheatState::WARNING, 0);
            } else {
                if (reg_.all_of<OverheatComponent>(ent)) {
                    reg_.remove<OverheatComponent>(ent);
                }
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // Pass 3: Environment cooling
    // ═══════════════════════════════════════════════════════════════════
    {
        auto cool_view = reg_.view<HeatIntakeComponent, Position>();
        for (auto ent : cool_view) {
            auto& hic = cool_view.get<HeatIntakeComponent>(ent);
            auto& pos = cool_view.get<Position>(ent);
            if (hic.heat_stored <= 0) continue;

            bool adjacent_to_water = false;
            for (auto& d : dirs) {
                int32_t nx = static_cast<int32_t>(pos.x) + d[0];
                int32_t ny = static_cast<int32_t>(pos.y) + d[1];
                int32_t nz = static_cast<int32_t>(pos.z) + d[2];
                if (nx < 0 || ny < 0 || nz < 0) continue;

                auto neighbor_view = reg_.view<const Position>();
                for (auto neighbor : neighbor_view) {
                    auto& np = neighbor_view.get<const Position>(neighbor);
                    if (static_cast<int32_t>(np.x) != nx ||
                        static_cast<int32_t>(np.y) != ny ||
                        static_cast<int32_t>(np.z) != nz) continue;
                    auto* block = reg_.try_get<Block>(neighbor);
                    if (block && block->id == 9) { adjacent_to_water = true; break; }
                }
                if (adjacent_to_water) break;
            }

            float cooling = HeatConstants::ENVIRONMENT_COOLING_RATE;
            if (adjacent_to_water) cooling *= HeatConstants::WATER_COOLING_MULTIPLIER;

            int32_t cool_amount = static_cast<int32_t>(cooling);
            if (cool_amount > hic.heat_stored) cool_amount = hic.heat_stored;
            hic.heat_stored -= cool_amount;

            if (auto* energy = reg_.try_get<EnergyStorage>(ent)) {
                if (energy->type == EnergyType::HEAT) {
                    energy->current = hic.heat_stored;
                }
            }
        }
    }
}

} // namespace simcore
