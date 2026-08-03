#include "DrillSystem.h"
#include "../components/ItemEnergyStorage.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <algorithm>
#include "../../world_generator/OreTypes.h"

namespace simcore {

// Ore block IDs — from items.csv (source of truth)
static constexpr uint16_t kOreBlocks[] = {
    ORE_IRON, ORE_GOLD, ORE_TIN, ORE_COPPER, ORE_URANIUM, ORE_QUARTZ,
    ORE_COAL, ORE_REDSTONE, ORE_LAPIS, ORE_DIAMOND, ORE_ELECTRUM
};

// Layer order: 0, -1, +1, -2, +2, -3, +3...
static int32_t layerOffset(int32_t layerIndex) {
    if (layerIndex == 0) return 0;
    return (layerIndex % 2 == 1) ? -(layerIndex + 1) / 2 : layerIndex / 2;
}

// =========================================================================
// Construction
// =========================================================================

DrillSystem::DrillSystem(entt::registry& reg,
                         std::shared_ptr<IBlockRepository> blockRepo,
                         std::shared_ptr<IEventPublisher> events,
                         std::shared_ptr<PipeEnergyClient> pipeClient)
    : reg_(reg), blockRepo_(std::move(blockRepo)),
      events_(std::move(events)), pipeClient_(std::move(pipeClient))
{
}

// =========================================================================
// Static helpers
// =========================================================================

bool DrillSystem::isOreBlock(uint16_t block_id) {
    for (auto id : kOreBlocks) {
        if (id == block_id) return true;
    }
    return false;
}

uint16_t DrillSystem::oreToDrop(uint16_t oreBlockId) {
    switch (oreBlockId) {
    case ORE_IRON:      return ItemId::pack("0:110:1");
    case ORE_GOLD:      return ItemId::pack("0:110:2");
    case ORE_TIN:       return ItemId::pack("0:110:3");
    case ORE_ELECTRUM:  return ItemId::pack("0:110:4");
    case ORE_COPPER:    return ItemId::pack("0:1110:1:0:1");
    case ORE_URANIUM:   return ItemId::pack("0:110:5");
    case ORE_QUARTZ:    return ItemId::pack("0:1110:1:1:2");
    case ORE_COAL:      return ItemId::pack("0:11110:2");
    case ORE_REDSTONE:  return 0;
    case ORE_LAPIS:     return 0;
    case ORE_DIAMOND:   return 0;
    default:            return 0;
    }
}

void DrillSystem::getSpiralOffset(int32_t n, int32_t& dx, int32_t& dz) {
    const int32_t dirDx[] = {1, 0, -1, 0};
    const int32_t dirDz[] = {0, 1, 0, -1};
    int32_t x = 0, z = 0;
    int32_t step = 1, dir = 0, count = 0;
    while (count <= n) {
        for (int32_t i = 0; i < 2 && count <= n; ++i) {
            for (int32_t j = 0; j < step && count <= n; ++j) {
                x += dirDx[dir]; z += dirDz[dir];
                if (count == n) { dx = x; dz = z; return; }
                ++count;
            }
            dir = (dir + 1) % 4;
        }
        ++step;
    }
    dx = 0; dz = 0;
}

// =========================================================================
// tick() — main entry point
// =========================================================================

void DrillSystem::tick(float /*dt*/) {
    // Drills draw energy from the drill tool item held in their inventory
    // (see phaseEnergyCheck); the machine-level EnergyStorage is optional and
    // only used as a fallback when no tool/container is present.
    auto view = reg_.view<DrillComponent>();

    for (auto ent : view) {
        auto& drill = view.get<DrillComponent>(ent);
        auto* container = reg_.try_get<InventoryContainer>(ent);
        auto* energy = reg_.try_get<EnergyStorage>(ent);

        switch (drill.state) {
        case DrillState::IDLE:
            drill.searchLayer = 0;
            drill.searchIndex = 0;
            drill.state = DrillState::SEARCHING;
            [[fallthrough]];

        case DrillState::SEARCHING:
            phaseSearch(ent, drill);
            break;

        case DrillState::MINING:
            // phaseEnergyCheck drains the drill tool's item energy; when the
            // tool is empty or no energy source is available it returns false
            // and transitions the drill out of MINING, so we skip phaseMine.
            if (phaseEnergyCheck(ent, drill) &&
                drill.state == DrillState::MINING) {
                phaseMine(ent, drill);
            }
            break;

        case DrillState::OUTPUT_FULL:
            if (drill.outputBuffer.size() < DrillComponent::kMaxOutputSize) {
                drill.state = DrillState::SEARCHING;
            }
            break;
        }

        float progress = 0.0f;
        if (drill.state == DrillState::MINING && drill.miningTicksTotal > 0) {
            progress = 1.0f - static_cast<float>(drill.miningProgress)
                       / static_cast<float>(drill.miningTicksTotal);
        }

        // Report energy: item-tool energy when a tool is equipped, otherwise
        // the machine EnergyStorage (or 0 when neither exists).
        uint32_t reportEnergy = 0, reportCapacity = 0;
        if (container) {
            int32_t slotIdx = findToolSlot(*container);
            if (slotIdx >= 0) {
                const auto& slot = container->slots[slotIdx];
                simulation_core::ItemStack toolStack{slot.item_id, slot.count,
                                                     slot.meta};
                reportEnergy =
                    static_cast<uint32_t>(std::max(getToolEnergy(toolStack), 0));
                auto it = TOOL_ENERGY_DEFS.find(slot.item_id);
                if (it != TOOL_ENERGY_DEFS.end())
                    reportCapacity = static_cast<uint32_t>(it->second.capacity);
            }
        }
        if (reportCapacity == 0 && energy) {
            reportEnergy = static_cast<uint32_t>(energy->current);
            reportCapacity = static_cast<uint32_t>(energy->capacity);
        }

        events_->publishBlockEntityUpdate(
            drill.x, drill.y, drill.z, 0, {}, progress, reportEnergy,
            energy ? energy->type : EnergyType::ELECTRICITY, reportCapacity);
    }
}

// =========================================================================
// Phase 1: Energy check
// =========================================================================

bool DrillSystem::phaseEnergyCheck(entt::entity ent, DrillComponent& drill) {
    // Primary path: draw energy from the drill tool item (item ids 90-94) held
    // in the drill machine's inventory. Tool energy lives in the item's meta,
    // so a successful drain is written back into the container slot to persist.
    if (auto* container = reg_.try_get<InventoryContainer>(ent)) {
        int32_t slotIdx = findToolSlot(*container);
        if (slotIdx >= 0) {
            auto& slot = container->slots[slotIdx];
            simulation_core::ItemStack toolStack{slot.item_id, slot.count,
                                                 slot.meta};
            if (consumeToolEnergy(toolStack, drill.energyPerTick)) {
                slot.meta = toolStack.meta; // persist drained tool energy
                return true;
            }
            // Tool lacks the required EU: abort mining, resume ore search.
            spdlog::debug(
                "[Drill] entity {} tool item {} has insufficient energy; "
                "aborting mining",
                static_cast<uint32_t>(ent), slot.item_id);
            drill.state = DrillState::SEARCHING;
            return false;
        }
    }

    // Fallback: machine-level EnergyStorage. Preserves the legacy behavior so
    // drills wired without an item inventory still operate as before.
    if (auto* energy = reg_.try_get<EnergyStorage>(ent)) {
        int32_t consumed = energy->consumeEnergy(drill.energyPerTick);
        if (consumed < drill.energyPerTick && pipeClient_) {
            pipeClient_->sendConsumeRequest(
                static_cast<uint64_t>(ent), drill.x, drill.y, drill.z,
                static_cast<int32_t>(EnergyType::ELECTRICITY),
                drill.energyPerTick);
        }
        return true;
    }

    // No energy source at all: cannot mine.
    drill.state = DrillState::SEARCHING;
    return false;
}

int32_t DrillSystem::findToolSlot(const InventoryContainer& container) {
    for (size_t i = 0; i < container.slots.size(); ++i) {
        if (TOOL_ENERGY_DEFS.find(container.slots[i].item_id) !=
            TOOL_ENERGY_DEFS.end()) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// =========================================================================
// Phase 2: Spiral BFS ore search
// =========================================================================

void DrillSystem::phaseSearch(entt::entity ent, DrillComponent& drill) {
    auto it = pendingSearches_.find(ent);
    int32_t pending = (it != pendingSearches_.end()) ? it->second : 0;
    if (pending >= 2) return;

    int32_t sent = pending;
    constexpr int32_t kMaxPerTick = 2;

    while (sent < kMaxPerTick) {
        int32_t dx, dz;
        getSpiralOffset(drill.searchIndex, dx, dz);
        int32_t dy = layerOffset(drill.searchLayer);
        int32_t wx = drill.x + dx, wy = drill.y + dy, wz = drill.z + dz;

        if (dx == 0 && dy == 0 && dz == 0) {
            drill.searchIndex++;
            continue;
        }

        blockRepo_->getBlock(wx, wy, wz,
            [this, ent, wx, wy, wz](const BlockData& block) {
                onSearchBlockResult(ent, wx, wy, wz, block);
            });

        sent++;
        drill.searchIndex++;
        if (drill.searchIndex >= 440) {
            drill.searchIndex = 0;
            drill.searchLayer++;
        }
    }

    pendingSearches_[ent] = sent;
}

void DrillSystem::onSearchBlockResult(entt::entity ent, int32_t wx, int32_t wy,
                                       int32_t wz, const BlockData& block) {
    auto it = pendingSearches_.find(ent);
    if (it != pendingSearches_.end()) {
        if (--it->second <= 0) pendingSearches_.erase(it);
    }

    if (!reg_.valid(ent)) return;
    auto* drill = reg_.try_get<DrillComponent>(ent);
    if (!drill || drill->state != DrillState::SEARCHING) return;
    if (!isOreBlock(block.block_id)) return;

    drill->targetX = wx;
    drill->targetY = wy;
    drill->targetZ = wz;
    drill->miningTicksTotal = DrillComponent::calcMiningTicks(drill->tier);
    drill->miningProgress = drill->miningTicksTotal;
    drill->state = DrillState::MINING;

    spdlog::debug("[Drill] entity {} found ore {} at ({},{},{})",
                  static_cast<uint32_t>(ent), block.block_id, wx, wy, wz);
}

// =========================================================================
// Phase 3: Mining progress
// =========================================================================

void DrillSystem::phaseMine(entt::entity ent, DrillComponent& drill) {
    drill.miningProgress--;
    if (drill.miningProgress > 0) return;

    blockRepo_->setBlockCAS(
        drill.targetX, drill.targetY, drill.targetZ,
        0xFFFF, 0, 0,
        [this, ent, drill](const CASResult& result) {
            onMineComplete(ent, drill, result);
        });
}

void DrillSystem::onMineComplete(entt::entity ent, const DrillComponent& drill,
                                  const CASResult& result) {
    if (!reg_.valid(ent)) return;
    auto* d = reg_.try_get<DrillComponent>(ent);
    if (!d) return;

    if (result.status != 0) {
        d->state = DrillState::SEARCHING;
        return;
    }

    d->outputBuffer.emplace_back(oreToDrop(result.block_id), 1);

    spdlog::debug("[Drill] mined block_id={} at ({},{},{})",
                  result.block_id, drill.targetX, drill.targetY, drill.targetZ);

    d->state = (d->outputBuffer.size() >= DrillComponent::kMaxOutputSize)
               ? DrillState::OUTPUT_FULL : DrillState::SEARCHING;
}

} // namespace simcore
