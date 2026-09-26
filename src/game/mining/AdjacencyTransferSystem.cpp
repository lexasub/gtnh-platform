#include "AdjacencyTransferSystem.h"
#include <engine/sim/components/Block.h>
#include <engine/sim/components/HeatIntakeComponent.h>
#include <game/machines/HeatSlowComponent.h>
#include <engine/sim/components/MachineComponent.h>
#include <game/machines/OverheatComponent.h>
#include <engine/sim/components/Position.h>
#include "game/machines/HeatConstants.h"
#include <spdlog/spdlog.h>

#include <engine/registry/ItemId.h>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace simcore {

AdjacencyTransferSystem::AdjacencyTransferSystem(entt::registry& reg,
                                                  MachineRegistry& machineRegistry,
                                                  std::shared_ptr<IEventPublisher> events)
    : reg_(reg), machineRegistry_(machineRegistry), events_(std::move(events))
{
}

namespace {

// 21 bits of x, 20 of y, 21 of z in one 64-bit key, so a neighbour lookup is a
// hash probe rather than a scan. The same shape the pass-3 water index uses.
constexpr uint64_t packPos(uint32_t x, uint32_t y, uint32_t z) {
    return (static_cast<uint64_t>(x) << 42) | (static_cast<uint64_t>(y) << 21) |
           static_cast<uint64_t>(z);
}

} // namespace

void AdjacencyTransferSystem::tick(float /*dt*/) {
    auto view = reg_.view<MachineComponent, EnergyStorage, Position>();

    // ═══════════════════════════════════════════════════════════════════
    // Pass 1: Heat transfer (adjacent producer → consumer)
    // ═══════════════════════════════════════════════════════════════════

    struct HeatProducer {
        entt::entity entity;
        uint32_t x, y, z;
        EnergyStorage* energy;
    };

    static const int dirs[6][3] = {
        {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}
    };

    // gp-c41q: index every heat SOURCE, not only the ones holding heat right
    // now. The old index was filtered on `current > 0`, which is correct for
    // "who has heat to give at snapshot time" but wrong for "who is able to give
    // heat at all": a relay that is COLD when the tick starts is refilled by an
    // upstream source during this very pass, and then has heat to forward. With
    // the snapshot filter it was simply not in the index, so the chain stopped
    // there and heat advanced one block per tick.
    //
    // The `current <= 0` check stays, per donor, at the point of transfer — the
    // index now says "this block can donate", and the live read of
    // prod.energy->current says "and right now it has something to donate".
    std::unordered_map<uint64_t, HeatProducer> producersByPos;
    for (auto ent : view) {
        auto& mc = view.get<MachineComponent>(ent);
        auto& energy = view.get<EnergyStorage>(ent);
        auto& pos = view.get<Position>(ent);
        if (energy.type != EnergyType::HEAT) continue;
        if (!machineRegistry_.IsHeatSource(mc.machine_id)) continue;
        // operator[], not emplace: the old code move-assigned into the slot, so
        // the LAST source written at a coordinate won — and because the view
        // runs in reverse creation order, that was the FIRST-created one. Two
        // producers at one world position is a latent defect (world positions
        // are unique in practice), pinned by …two_sources_at_one_position_.
        producersByPos[packPos(pos.x, pos.y, pos.z)] =
            HeatProducer{ent, pos.x, pos.y, pos.z, &energy};
    }

    if (!producersByPos.empty()) {
        // ── gp-c41q: order the sinks by DISTANCE FROM A SOURCE, not by ECS
        // creation order ────────────────────────────────────────────────────
        //
        // The pass used to visit sinks in raw view order. EnTT yields that view
        // in REVERSE creation order, so whether a hop completed in a tick was
        // decided by which of two machines happened to be created last — an ECS
        // bookkeeping detail, not a game rule. A cold relay is not even in the
        // producer snapshot (current > 0 is required at snapshot time), so a
        // cold chain advanced exactly ONE BLOCK PER TICK: a generator feeding a
        // three-block coil run left the drill heat cold for a full tick while
        // the charge piled up in the block in front of it.
        //
        // The fix is a relaxation sweep in nearest-source-distance order: a
        // relay at distance k is visited only after every block at distance
        // k-1 has had its chance to refill it, so the heat it takes this tick is
        // available to the block at distance k+1 in the SAME tick.
        //
        // The BFS expands through a node only when that node is a heat SOURCE,
        // because pass 1 only ever moves heat OUT of a source — a pure sink
        // cannot relay, and treating it as a conduit would hand heat to a block
        // the old pass could never have reached.
        //
        // A sink the BFS never reaches has no finite distance and is skipped,
        // exactly as before. Ties (two blocks at the same distance) keep the
        // view order, so the result stays deterministic.

        // Every candidate sink, in view order, with its source-distance.
        struct SinkVisit {
            entt::entity entity;
            uint32_t x, y, z;
            int32_t distance;  // -1 == unreachable
        };
        std::vector<SinkVisit> sinks;
        for (auto ent : view) {
            auto& mc = view.get<MachineComponent>(ent);
            auto& energy = view.get<EnergyStorage>(ent);
            auto& pos = view.get<Position>(ent);
            if (energy.type != EnergyType::HEAT) continue;
            if (!machineRegistry_.IsHeatSink(mc.machine_id)) continue;
            sinks.push_back({ent, pos.x, pos.y, pos.z, -1});
        }

        // Which positions hold a heat source? These are the BFS seeds AND the
        // only nodes the BFS may expand through, because pass 1 only ever moves
        // heat out of a source. A machine that is both a source and a sink (a
        // relay) appears here even while it is cold, which is exactly what lets
        // heat traverse a cold chain in a single tick.
        std::unordered_set<uint64_t> sourcePos;
        sourcePos.reserve(sinks.size() * 2 + 8);
        for (auto ent : view) {
            auto& mc = view.get<MachineComponent>(ent);
            auto& energy = view.get<EnergyStorage>(ent);
            auto& pos = view.get<Position>(ent);
            if (energy.type != EnergyType::HEAT) continue;
            if (!machineRegistry_.IsHeatSource(mc.machine_id)) continue;
            sourcePos.insert(packPos(pos.x, pos.y, pos.z));
        }

        // Multi-source BFS. A heat SOURCE is enqueued and expanded; a pure SINK
        // is assigned a distance but NOT enqueued, because pass 1 has no path by
        // which heat could leave it.
        //
        // THE SEEDS ARE THE SOURCES THAT ACTUALLY HOLD HEAT (`current > 0`),
        // while EXPANSION still passes through every source. That distinction is
        // the whole fix: a relay that is cold when the tick starts is not a
        // seed, so it sits at its true distance from the nearest live source
        // instead of being pinned to distance 0. Seeding from every source
        // instead would put a whole chain of relays at distance 0, the
        // stable-sort tie-break would fall back to view order, and the chain
        // would advance one block per tick again — the original defect, reached
        // through a different route.
        //
        // Every position is recorded at most once, so this is O(sources +
        // neighbours) rather than O(nodes^2).
        std::unordered_set<uint64_t> sinkPos;
        sinkPos.reserve(sinks.size() * 2 + 8);
        for (const auto& sv : sinks) sinkPos.insert(packPos(sv.x, sv.y, sv.z));

        std::unordered_map<uint64_t, int32_t> distByPos;
        distByPos.reserve(sourcePos.size() * 2 + sinks.size() + 8);
        struct Frontier {
            uint32_t x, y, z;
        };
        std::vector<Frontier> bfs;
        bfs.reserve(sourcePos.size());
        for (const auto& ent : view) {
            auto& energy = view.get<EnergyStorage>(ent);
            auto& pos = view.get<Position>(ent);
            if (energy.type != EnergyType::HEAT) continue;
            if (!machineRegistry_.IsHeatSource(
                    view.get<MachineComponent>(ent).machine_id)) {
                continue;
            }
            if (energy.current <= 0) continue;  // a cold source is not a seed
            if (!distByPos.emplace(packPos(pos.x, pos.y, pos.z), 0).second) continue;
            bfs.push_back({pos.x, pos.y, pos.z});
        }
        for (size_t head = 0; head < bfs.size(); ++head) {
            const int32_t d =
                distByPos.find(packPos(bfs[head].x, bfs[head].y, bfs[head].z))->second;
            for (const auto& dir : dirs) {
                const int64_t nx = static_cast<int64_t>(bfs[head].x) + dir[0];
                const int64_t ny = static_cast<int64_t>(bfs[head].y) + dir[1];
                const int64_t nz = static_cast<int64_t>(bfs[head].z) + dir[2];
                if (nx < 0 || ny < 0 || nz < 0) continue;
                const uint64_t nkey = packPos(static_cast<uint32_t>(nx),
                                             static_cast<uint32_t>(ny),
                                             static_cast<uint32_t>(nz));
                // A source recurses; a sink is a dead end but still receives.
                const bool isSource = sourcePos.find(nkey) != sourcePos.end();
                if (!isSource && sinkPos.find(nkey) == sinkPos.end()) continue;
                if (!distByPos.emplace(nkey, d + 1).second) continue;
                if (isSource) {
                    bfs.push_back({static_cast<uint32_t>(nx), static_cast<uint32_t>(ny),
                                   static_cast<uint32_t>(nz)});
                }
            }
        }

        for (auto& sink : sinks) {
            auto it = distByPos.find(packPos(sink.x, sink.y, sink.z));
            if (it == distByPos.end()) continue;  // unreachable: skip, as before
            sink.distance = it->second;
        }

        // Stable sort by distance: equal distances keep view order, so the
        // sweep is deterministic and the tie-break is documented.
        std::stable_sort(sinks.begin(), sinks.end(),
                         [](const SinkVisit& a, const SinkVisit& b) {
                             return a.distance < b.distance;
                         });

        for (const auto& sv : sinks) {
            auto& mc = view.get<MachineComponent>(sv.entity);
            auto& energy = view.get<EnergyStorage>(sv.entity);
            if (energy.current >= energy.capacity) continue;

            int32_t needed = energy.capacity - energy.current;
            if (needed <= 0) continue;

            for (auto& d : dirs) {
                int32_t nx = static_cast<int32_t>(sv.x) + d[0];
                int32_t ny = static_cast<int32_t>(sv.y) + d[1];
                int32_t nz = static_cast<int32_t>(sv.z) + d[2];
                if (nx < 0 || ny < 0 || nz < 0) continue;

                auto prodIt = producersByPos.find(packPos(
                    static_cast<uint32_t>(nx),
                    static_cast<uint32_t>(ny),
                    static_cast<uint32_t>(nz)));
                if (prodIt == producersByPos.end()) continue;
                auto& prod = prodIt->second;
                if (prod.entity == sv.entity) continue;
                if (prod.energy->current <= 0) continue;

                int32_t available = prod.energy->current;
                int32_t transfer = std::min(needed, available);
                if (transfer <= 0) continue;

                prod.energy->current -= transfer;
                energy.current += transfer;
                needed -= transfer;

                // Sync HeatIntakeComponent
                if (auto* hic = reg_.try_get<HeatIntakeComponent>(sv.entity)) {
                    hic->heat_stored = energy.current;
                }

                spdlog::debug("[AdjTransfer] {} → {} transferred {} {} ({},{},{})",
                             mc.machine_id, static_cast<uint32_t>(sv.entity),
                             transfer, MachineRegistry::EnergyLabel(energy.type),
                             sv.x, sv.y, sv.z);

                if (needed <= 0) break;
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // Pass 2: Overheat detection
    // ═══════════════════════════════════════════════════════════════════
    {
        // The gate is `MachineComponent` with a nonzero mb_id — i.e. a live
        // multiblock controller anchor. This used to be
        // `view<HeatIntakeComponent, MultiblockController>()` (gp-wjwr), and
        // that view could never match: a MultiblockController ECS component is
        // never emplaced anywhere in production, because SimulationEngine owns
        // controllers in a plain
        // `std::unordered_map<uint64_t, MultiblockController> controllers_`
        // (SimulationEngine.h:108) which EBFSystem / LCRSystem /
        // LargeBoilerSystem mutate in place — an ECS mirror would be a second
        // source of truth free to desync from it. MachineComponent::mb_id is
        // the field the engine already keeps correct: set on formation
        // (SimulationEngine.cpp:309), refreshed on every block echo (:370), and
        // removed with the MachineComponent when the controller is destroyed
        // (destroyController, :82). Member blocks (casing/coil) are not
        // machines, so only the anchor can have mb_id != 0.
        auto oh_view = reg_.view<HeatIntakeComponent, MachineComponent>();
        for (auto ent : oh_view) {
            if (oh_view.get<MachineComponent>(ent).mb_id == 0) continue;
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
