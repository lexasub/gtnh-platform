#include "ECS/SimulationEngine.h"
#include "Common/xyz.h"
#include <common/ItemId.h>
#include <spdlog/spdlog.h>
#include <algorithm>

constexpr bool isInfraBlock(uint16_t id) {
    return id == ItemId::pack("1111:10:0") || id == ItemId::pack("1111:10:1")
        || id == ItemId::pack("1111:10:2") || id == ItemId::pack("1111:10:3")
        || id == ItemId::pack("1111:01:0") || id == ItemId::pack("1111:01:1")
        || id == ItemId::pack("1111:01:2") || id == ItemId::pack("1111:01:3")
        || id == ItemId::pack("1111:01:4") || id == ItemId::pack("1111:01:5")
        || id == ItemId::pack("1110:11:0")|| id == ItemId::pack("1110:11:1");
}

namespace simcore {

// Определение паттерна электролизёра (3x3x3 сетка, все 27 блоков)
const std::vector<std::tuple<int32_t, int32_t, int32_t>> ELECTROLYSER_PATTERN = {
    {-1,-1,-1}, {-1, 0,-1}, {-1, 1,-1},
    { 0,-1,-1}, { 0, 0,-1}, { 0, 1,-1},
    {-1,-1, 0}, {-1, 0, 0}, {-1, 1, 0},
    { 0,-1, 0}, { 0, 0, 0}, { 0, 1, 0},
    {-1,-1, 1}, {-1, 0, 1}, {-1, 1, 1},
    { 0,-1, 1}, { 0, 0, 1}, { 0, 1, 1},
    {-1,-1, 2}, {-1, 0, 2}, {-1, 1, 2},
    { 0,-1, 2}, { 0, 0, 2}, { 0, 1, 2}
};

entt::entity SimulationEngine::findEntityAt(uint32_t x, uint32_t y, uint32_t z) const
{
    auto view = reg_.view<const Position>();
    for (auto entity : view) {
        auto& pos = view.get<const Position>(entity);
        if (pos.x == x && pos.y == y && pos.z == z) {
            return entity;
        }
    }
    return entt::null;
}

void SimulationEngine::removeBlockFromController(uint32_t mb_id, uint32_t x, uint32_t y, uint32_t z)
{
    auto it = controllers_.find(mb_id);
    if (it != controllers_.end()) {
        uint32_t packed = xyz(x, y, z);
        auto& blocks = it->second.blocks;
        blocks.erase(std::ranges::remove(blocks, packed).begin(), blocks.end());
        spdlog::debug("[ECS] Block ({},{},{}) removed from controller #{}", x, y, z, mb_id);
    }
}

void SimulationEngine::onBlockChanged(uint32_t x, uint32_t y, uint32_t z,
                                      uint16_t block_id, uint8_t meta, uint32_t mb_id)
{
    auto entity = findEntityAt(x, y, z);
    bool exists = (entity != entt::null);

    if (block_id == 0) {
        if (exists) {
            auto* blk = reg_.try_get<Block>(entity);
            if (blk && blk->mb_id != 0) {
                removeBlockFromController(blk->mb_id, x, y, z);
            }
            // Keep Position so findEntityAt can find this entity when a
            // new block is placed. Avoids entity ID recycling which can
            // cause EnTT sparse_set stale-entry assertions.
            reg_.remove<Block>(entity);
            if (reg_.all_of<MachineComponent>(entity)) {
                reg_.remove<MachineComponent>(entity);
                reg_.remove<RecipeProgress>(entity);
                reg_.remove<InventoryContainer>(entity);
                reg_.remove<EnergyStorage>(entity);
                spdlog::debug("[ECS] Removed machine components from cleared entity at ({},{},{})", x, y, z);
            }
            spdlog::debug("[ECS] Cleared block entity at ({},{},{})", x, y, z);
        }
        return;
    }

    if (!exists) {
        entity = reg_.create();
        reg_.emplace<Position>(entity, x, y, z);
    }

    bool was_machine = false;
    {
        auto* old_block = reg_.try_get<Block>(entity);
        if (old_block && old_block->mb_id != 0 && old_block->mb_id != mb_id) {
            removeBlockFromController(old_block->mb_id, x, y, z);
        }
        was_machine = reg_.all_of<MachineComponent>(entity);
    }

    auto& block = reg_.get_or_emplace<Block>(entity, block_id, meta, mb_id);
    block.id = block_id;
    block.meta = meta;
    block.mb_id = mb_id;

    if (mb_id != 0) {
        removeBlockFromController(mb_id, x, y, z);
    }

    bool is_machine = isMachineBlock(block_id);

    if (was_machine && !is_machine) {
        reg_.remove<MachineComponent>(entity);
        reg_.remove<RecipeProgress>(entity);
        reg_.remove<InventoryContainer>(entity);
        reg_.remove<EnergyStorage>(entity);
        spdlog::debug("[ECS] Removed machine components from entity at ({},{},{})", x, y, z);

    } else if (!was_machine && is_machine) {
        auto& mc = reg_.emplace_or_replace<MachineComponent>(entity, block_id, mb_id, x, y, z, next_machine_id_++);
        if (mb_id != 0) {
            mc.managed_externally = true;
            if (onMachineCreated) {
                onMachineCreated(static_cast<int32_t>(x),
                                 static_cast<int32_t>(y),
                                 static_cast<int32_t>(z),
                                 block_id);
            }
        }
        reg_.emplace_or_replace<RecipeProgress>(entity);
        InventoryContainer container;
        container.entity_type = (mb_id != 0) ? 2 : 1;
        container.slot_count = defaultMachineSlotCount(block_id);
        container.slots.resize(container.slot_count);
        reg_.emplace_or_replace<InventoryContainer>(entity, std::move(container));
        EnergyType etype = EnergyType::ELECTRICITY;
        int capacity = 10000;
        int maxInput = 32;
        int maxOutput = 32;
        int tier = 1;
        
        if (machine_registry_) {
            if (auto* info = machine_registry_->Get(block_id)) {
                if (info->energy_in.has_value()) etype = info->energy_in.value();
                else if (info->energy_out.has_value()) etype = info->energy_out.value();
                capacity = info->capacity;
                maxInput = info->maxInput;
                maxOutput = info->maxOutput;
                tier = info->tier;
            }
        }
        reg_.emplace_or_replace<EnergyStorage>(entity, capacity, 0, maxInput, maxOutput, tier, etype);

        if (etype == EnergyType::HEAT) {
            reg_.emplace_or_replace<HeatIntakeComponent>(entity);
        }

        spdlog::debug("[ECS] Created machine entity #{} type={} at ({},{},{})",
                      next_machine_id_ - 1, block_id, x, y, z);

    } else if (is_machine) {
        auto& mc = reg_.get<MachineComponent>(entity);
        mc.machine_id = block_id;
        mc.mb_id = mb_id;
        mc.x = x; mc.y = y; mc.z = z;

        bool should_external = (mb_id != 0);
        if (mc.managed_externally != should_external) {
            mc.managed_externally = should_external;
            if (should_external && onMachineCreated) {
                onMachineCreated(static_cast<int32_t>(x),
                                 static_cast<int32_t>(y),
                                 static_cast<int32_t>(z),
                                 block_id);
            }
        }
        spdlog::debug("[ECS] Updated machine entity at ({},{},{}) type={}", x, y, z, block_id);
    }
}

uint64_t SimulationEngine::matchElectrolyser(uint32_t anchor_x, uint32_t anchor_y,
                                             uint32_t anchor_z, uint64_t controller_id)
{
    // Проверяем, что все позиции паттерна заняты блоками с id == 1 (камень)
    for (const auto& [dx, dy, dz] : ELECTROLYSER_PATTERN) {
        int32_t px = static_cast<int32_t>(anchor_x) + dx;
        int32_t py = static_cast<int32_t>(anchor_y) + dy;
        int32_t pz = static_cast<int32_t>(anchor_z) + dz;

        if (px < 0 || py < 0 || pz < 0) return 0;

        uint32_t ux = static_cast<uint32_t>(px);
        uint32_t uy = static_cast<uint32_t>(py);
        uint32_t uz = static_cast<uint32_t>(pz);

        bool found = false;
        auto view = reg_.view<const Position, const Block>();
        for (auto entity : view) {
            auto [pos, blk] = view.get(entity);
            if (pos.x == ux && pos.y == uy && pos.z == uz) {
                if (blk.id != 1) return 0;
                found = true;
                break;
            }
        }
        if (!found) return 0;
    }

    // Собираем упакованные координаты всех блоков мультиблока
    std::vector<uint32_t> blocks;
    blocks.reserve(ELECTROLYSER_PATTERN.size());
    for (const auto& [dx, dy, dz] : ELECTROLYSER_PATTERN) {
        blocks.push_back(xyz(
            static_cast<uint32_t>(static_cast<int32_t>(anchor_x) + dx),
            static_cast<uint32_t>(static_cast<int32_t>(anchor_y) + dy),
            static_cast<uint32_t>(static_cast<int32_t>(anchor_z) + dz)
        ));
    }

    registerController(controller_id, anchor_x, anchor_y, anchor_z, blocks);
    spdlog::info("[ECS] Matched electrolyser controller #{} at anchor ({},{},{})",
                 controller_id, anchor_x, anchor_y, anchor_z);
    return controller_id;
}

void SimulationEngine::registerController(uint64_t id, uint32_t x, uint32_t y, uint32_t z,
                                          const std::vector<uint32_t>& blocks)
{
    controllers_.emplace(id, MultiblockController(id, x, y, z, blocks));
    spdlog::info("[ECS] Registered controller #{} at ({},{},{}) with {} blocks",
                 id, x, y, z, blocks.size());
}

bool SimulationEngine::isControllerActive(uint64_t id) const
{
    return controllers_.find(id) != controllers_.end();
}

const MultiblockController& SimulationEngine::getController(uint64_t id) const
{
    static const MultiblockController empty;
    auto it = controllers_.find(id);
    return it != controllers_.end() ? it->second : empty;
}

void SimulationEngine::unregisterController(uint64_t id)
{
    controllers_.erase(id);
    spdlog::info("[ECS] Unregistered controller #{}", id);
}

void SimulationEngine::registerSystem(std::unique_ptr<ISystem> system)
{
    systems_.push_back(std::move(system));
}

void SimulationEngine::tickAll(float dt)
{
    for (auto& sys : systems_) {
        sys->tick(dt);
    }
}

bool SimulationEngine::isMachineBlock(uint16_t block_id) const
{
    if (isInfraBlock(block_id)) return false;
    if (machine_registry_) {
        return machine_registry_->IsMachine(block_id);
    }
    return false;
}

uint32_t SimulationEngine::defaultMachineSlotCount(uint16_t block_id) const
{
    if (machine_registry_) {
        auto* info = machine_registry_->Get(block_id);
        if (info) return static_cast<uint32_t>(info->slots_in + info->slots_out);
    }
    return 0;
}

} // namespace simcore