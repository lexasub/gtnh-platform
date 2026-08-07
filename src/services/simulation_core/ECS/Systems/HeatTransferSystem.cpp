#include "HeatTransferSystem.h"
#include "../components/Block.h"
#include "../components/HeatIntakeComponent.h"
#include "../components/HeatSlowComponent.h"
#include "../components/MachineComponent.h"
#include "../components/MultiblockController.h"
#include "../components/OverheatComponent.h"
#include "../components/Position.h"
#include "HeatConstants.h"
#include <spdlog/spdlog.h>

#include "../../common/ItemId.h"
#include <unordered_map>
#include <unordered_set>

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
        if (!info) continue;
        bool isProducer = info->role == MachineRole::PRODUCER //TODO fix read real info
                       || (info->energy_out.has_value()
                        && info->energy_out.value() == EnergyType::HEAT);
        if (!isProducer) continue;
        if (energy.current <= 0) continue;
        producers.push_back({ent, pos.x, pos.y, pos.z, &energy});
    }

    static const int dirs[6][3] = {
        {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
    };

    if (!producers.empty()) {
        // Build spatial index for O(1) neighbour lookup
        auto packPos = [](uint32_t x, uint32_t y, uint32_t z) -> uint64_t {
            return (static_cast<uint64_t>(x) << 42) |
                   (static_cast<uint64_t>(y) << 21) |
                    static_cast<uint64_t>(z);
        };
        std::unordered_map<uint64_t, HeatProducer> producersByPos;
        producersByPos.reserve(producers.size());
        for (auto& prod : producers) {
            producersByPos[packPos(prod.x, prod.y, prod.z)] = std::move(prod);
        }

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

                auto prodIt = producersByPos.find(packPos(
                    static_cast<uint32_t>(nx),
                    static_cast<uint32_t>(ny),
                    static_cast<uint32_t>(nz)));
                if (prodIt == producersByPos.end()) continue;
                auto& prod = prodIt->second;
                if (prod.entity == ent) continue;
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
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // Pass 2: Overheat detection
    // ═══════════════════════════════════════════════════════════════════
    {
        auto oh_view = reg_.view<HeatIntakeComponent, MultiblockController>();
        for (auto ent : oh_view) {
            auto& hic = oh_view.get<HeatIntakeComponent>(ent);
            float r = hic.ratio();

            if (r >= HeatConstants::OVERHEAT_CRITICAL_THRESHOLD) {
                // Emplace only if absent; otherwise update .state in place so
                // ticks_at_critical accumulates across consecutive CRITICAL ticks
                // (ExplosionSystem counts down from EXPLOSION_DELAY_TICKS).
                if (auto* oh = reg_.try_get<OverheatComponent>(ent)) {
                    oh->state = OverheatState::CRITICAL;
                } else {
                    reg_.emplace<OverheatComponent>(ent, OverheatState::CRITICAL, 0);
                }
            } else if (r >= HeatConstants::OVERHEAT_WARNING_THRESHOLD) {
                if (auto* oh = reg_.try_get<OverheatComponent>(ent)) {
                    oh->state = OverheatState::WARNING;
                } else {
                    reg_.emplace<OverheatComponent>(ent, OverheatState::WARNING, 0);
                }
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
        // Pre-compute water block positions for O(1) neighbour lookup
        auto packPos = [](uint32_t x, uint32_t y, uint32_t z) -> uint64_t {
            return (static_cast<uint64_t>(x) << 42) |
                   (static_cast<uint64_t>(y) << 21) |
                    static_cast<uint64_t>(z);
        };
        std::unordered_set<uint64_t> waterPositions;
        {
            auto water_view = reg_.view<const Position, const Block>();
            for (auto w : water_view) {
                auto& wp = water_view.get<const Position>(w);
                auto& wb = water_view.get<const Block>(w);
                if (wb.id == ItemId::pack("0:0:9")) {
                    waterPositions.insert(packPos(wp.x, wp.y, wp.z));
                }
            }
        }

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

                if (waterPositions.contains(packPos(
                        static_cast<uint32_t>(nx),
                        static_cast<uint32_t>(ny),
                        static_cast<uint32_t>(nz)))) {
                    adjacent_to_water = true;
                    break;
                }
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
