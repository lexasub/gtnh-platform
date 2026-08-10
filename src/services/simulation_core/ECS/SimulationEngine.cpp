#include "ECS/SimulationEngine.h"
#include "ECS/Systems/RotareGeneratorSystem.h"
#include "Common/xyz.h"
#include <common/ItemId.h>
#include "multiblock_state_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>
#include <algorithm>

constexpr bool isInfraBlock(uint16_t id) {
    return ItemId::isPipe(id) || ItemId::isCable(id)
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

// Multiblock block lists are packed with x in the low 10 bits
// (PatternLibrary::collectBlocks). Common/xyz.h uses a DIFFERENT (x-high)
// layout — never use it for controller block lookups.
static uint32_t mbPack(uint32_t x, uint32_t y, uint32_t z) {
    return (x & 0x3FF) | ((y & 0x3FF) << 10) | ((z & 0x3FF) << 20);
}

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
        uint32_t packed = mbPack(x, y, z);
        auto& blocks = it->second.blocks;
        blocks.erase(std::ranges::remove(blocks, packed).begin(), blocks.end());
        spdlog::debug("[ECS] Block ({},{},{}) removed from controller #{}", x, y, z, mb_id);
    }
}

void SimulationEngine::destroyController(uint64_t id)
{
    auto it = controllers_.find(id);
    if (it == controllers_.end()) return;

    spdlog::info("[ECS] Multiblock controller #{} at ({},{},{}) destroyed",
                 id, it->second.x, it->second.y, it->second.z);

    // Persist final state. Player-initiated breaks already moved contents to
    // the player via the block-break guard (SetBlockCASHandler); world-initiated
    // clears keep contents inside the saved MultiblockState (task 2.2).
    if (onMultiblockSave) {
        auto blob = serializeMultiblock(id);
        if (!blob.empty()) onMultiblockSave(id, blob);
    }

    // Remove the controller entity's machine components (otherwise a stale
    // machine entity lingers when a structural block breaks the multiblock).
    auto ctrl_entity = findEntityAt(it->second.x, it->second.y, it->second.z);
    if (ctrl_entity != entt::null) {
        if (reg_.all_of<MachineComponent>(ctrl_entity)) reg_.remove<MachineComponent>(ctrl_entity);
        if (reg_.all_of<RecipeProgress>(ctrl_entity)) reg_.remove<RecipeProgress>(ctrl_entity);
        if (reg_.all_of<InventoryContainer>(ctrl_entity)) reg_.remove<InventoryContainer>(ctrl_entity);
        if (reg_.all_of<EnergyStorage>(ctrl_entity)) reg_.remove<EnergyStorage>(ctrl_entity);
        if (reg_.all_of<HeatIntakeComponent>(ctrl_entity)) reg_.remove<HeatIntakeComponent>(ctrl_entity);
    }

    controllers_.erase(it);
    if (onMultiblockDestroyed) onMultiblockDestroyed(id);
}

std::unordered_map<uint64_t, MultiblockController>::iterator
SimulationEngine::findControllerAt(uint32_t x, uint32_t y, uint32_t z)
{
    uint32_t packed = mbPack(x, y, z);
    for (auto it = controllers_.begin(); it != controllers_.end(); ++it) {
        if (std::ranges::find(it->second.blocks, packed) != it->second.blocks.end()) {
            return it;
        }
    }
    return controllers_.end();
}

bool SimulationEngine::isMultiblockBlockAt(uint32_t x, uint32_t y, uint32_t z) const
{
    uint32_t packed = mbPack(x, y, z);
    for (const auto& [id, ctrl] : controllers_) {
        if (std::ranges::find(ctrl.blocks, packed) != ctrl.blocks.end()) return true;
    }
    return false;
}

void SimulationEngine::collectControllerContents(const MultiblockController& ctrl,
                                                 std::vector<InventorySlot>& out) const
{
    auto entity = findEntityAt(ctrl.x, ctrl.y, ctrl.z);
    if (entity == entt::null) return;
    auto* inv = reg_.try_get<InventoryContainer>(entity);
    if (!inv) return;
    for (const auto& slot : inv->slots) {
        if (slot.item_id != 0) out.push_back(slot);
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
            uint32_t old_mb_id = blk ? blk->mb_id : 0;

            if (old_mb_id == 0) {
                // Pattern/structural blocks don't carry mb_id — find the owning
                // controller by position.
                auto owner = findControllerAt(x, y, z);
                if (owner != controllers_.end()) {
                    old_mb_id = static_cast<uint32_t>(owner->first);
                }
            }

            if (old_mb_id != 0) {
                if (controllers_.count(old_mb_id) != 0) {
                    destroyController(old_mb_id);
                } else {
                    removeBlockFromController(old_mb_id, x, y, z);
                }
            }

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
        }
        reg_.emplace_or_replace<RecipeProgress>(entity);
        InventoryContainer container;
        container.entity_type = (mb_id != 0) ? 2 : 1;
        container.slot_count = defaultMachineSlotCount(block_id);
        container.slots.resize(container.slot_count);
        spdlog::debug("[Diagnostic] Created InventoryContainer for block_id={} at ({},{},{}): slot_count={} slots.size()={}",
                      block_id, x, y, z, container.slot_count, container.slots.size());
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

        if (machine_registry_) {
            if (auto* info = machine_registry_->Get(block_id)) {
                if (info->energy_out.has_value() && info->energy_out.value() == EnergyType::STEAM) {
                    reg_.emplace_or_replace<SteamOutputComponent>(entity);
                }
            }
        }

        if (onMachineCreated) {
            onMachineCreated(static_cast<int32_t>(x),
                             static_cast<int32_t>(y),
                             static_cast<int32_t>(z),
                             block_id);
        }

        spdlog::debug("[ECS] Created machine entity #{} type={} at ({},{},{})",
                      next_machine_id_ - 1, block_id, x, y, z);

        if (mb_id == 0 && pattern_registry_.isControllerBlock(block_id)) {
            auto lookup = [this](uint32_t lx, uint32_t ly, uint32_t lz) -> uint16_t {
                auto view = reg_.view<const Position, const Block>();
                for (auto e : view) {
                    auto [pos, blk] = view.get(e);
                    if (pos.x == lx && pos.y == ly && pos.z == lz) return blk.id;
                }
                return 0;
            };
            auto result = pattern_registry_.matchAll(x, y, z, lookup);
            if (result.matched) {
                uint64_t ctrl_id = next_machine_id_++;
                registerController(ctrl_id, x, y, z, result.pattern_id, result.blocks);
                mc.managed_externally = true;
                mc.mb_id = static_cast<uint32_t>(ctrl_id);
                block.mb_id = static_cast<uint32_t>(ctrl_id);
                container.entity_type = 2;
                // onMachineCreated fires once above (after all components are
                // created) — no duplicate here.
                const auto* pat = pattern_registry_.getPattern(result.pattern_id);
                spdlog::info("[ECS] Matched multiblock '{}' #{} at ({},{},{})",
                             pat ? pat->name : "?", ctrl_id, x, y, z);
                if (onMultiblockCreated) {
                    onMultiblockCreated(ctrl_id, static_cast<int32_t>(x),
                                        static_cast<int32_t>(y),
                                        static_cast<int32_t>(z),
                                        static_cast<uint16_t>(result.pattern_id));
                }
                if (pat && !pat->hatches.empty()) {
                    auto hatches = pattern_registry_.findHatches(x, y, z, *pat, lookup);
                    auto& ctrl = controllers_[ctrl_id];
                    ctrl.hatches.resize(hatches.size());
                    uint16_t offset = 0;
                    for (size_t i = 0; i < hatches.size(); ++i) {
                        auto& hs = ctrl.hatches[i];
                        hs.type = hatches[i].type;
                        hs.world_x = static_cast<uint32_t>(hatches[i].world_x);
                        hs.world_y = static_cast<uint32_t>(hatches[i].world_y);
                        hs.world_z = static_cast<uint32_t>(hatches[i].world_z);
                        uint16_t slot_count = HatchSlot::kSlotsPerHatch(hs.type);
                        if (slot_count > 0) {
                            hs.slot_start = offset;
                            hs.slot_end = offset + slot_count;
                            offset += slot_count;
                        }
                    }
                    // Reorder: ITEM_IN first, then ITEM_OUT
                    std::vector<HatchSlot> ordered;
                    ordered.reserve(ctrl.hatches.size());
                    for (auto& hs : ctrl.hatches) {
                        if (hs.hasItemSlots()) ordered.push_back(hs);
                    }
                    for (auto& hs : ctrl.hatches) {
                        if (!hs.hasItemSlots()) ordered.push_back(hs);
                    }
                    ctrl.hatches = std::move(ordered);
                    // Re-assign offsets after reorder
                    offset = 0;
                    for (auto& hs : ctrl.hatches) {
                        if (hs.hasItemSlots()) {
                            hs.slot_start = offset;
                            hs.slot_end = offset + HatchSlot::kSlotsPerHatch(hs.type);
                            offset = hs.slot_end;
                        }
                    }
                    assignHatchSlots(ctrl, entity);
                }
            }
        }

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
                if (blk.id != ItemId::pack("0:0:1")) return 0;
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

    registerController(controller_id, anchor_x, anchor_y, anchor_z, 0, blocks);
    spdlog::info("[ECS] Matched electrolyser controller #{} at anchor ({},{},{})",
                 controller_id, anchor_x, anchor_y, anchor_z);
    return controller_id;
}

void SimulationEngine::registerController(uint64_t id, uint32_t x, uint32_t y, uint32_t z,
                                          uint32_t pattern_id,
                                          const std::vector<uint32_t>& blocks)
{
    controllers_.emplace(id, MultiblockController(id, x, y, z, pattern_id, blocks));
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

void SimulationEngine::assignHatchSlots(MultiblockController& ctrl, entt::entity entity)
{
    if (entity == entt::null) return;
    auto* inv = reg_.try_get<InventoryContainer>(entity);
    if (!inv) return;

    uint16_t total_hatch_slots = 0;
    for (const auto& hs : ctrl.hatches) {
        total_hatch_slots += HatchSlot::kSlotsPerHatch(hs.type);
    }

    uint32_t base_count = inv->slot_count;
    // If controller was already assigned slots, don't double-count
    // Reallocate to base + hatch slots
    uint32_t new_count = base_count + total_hatch_slots;
    if (new_count == inv->slot_count) return; // no change needed

    inv->slot_count = new_count;
    inv->slots.resize(new_count);
}

void SimulationEngine::getInputSlotRange(const MultiblockController& ctrl, int& slot_start, int& slot_end)
{
    slot_start = 0;
    slot_end = 0;
    for (const auto& hs : ctrl.hatches) {
        if (hs.type == HatchType::ITEM_IN) {
            slot_start = hs.slot_start;
            slot_end = hs.slot_end;
            break;
        }
    }
}

void SimulationEngine::getOutputSlotRange(const MultiblockController& ctrl, int& slot_start, int& slot_end)
{
    slot_start = 0;
    slot_end = 0;
    for (const auto& hs : ctrl.hatches) {
        if (hs.type == HatchType::ITEM_OUT) {
            slot_start = hs.slot_start;
            slot_end = hs.slot_end;
            break;
        }
    }
}

void SimulationEngine::registerMachineInteractionHandler(uint16_t machine_id, MachineInteractionHandler handler) {
    interaction_handlers_[machine_id] = std::move(handler);
}

void SimulationEngine::onMachineInteracted(int32_t x, int32_t y, int32_t z,
                                           uint16_t machine_id, uint64_t player_id) {
    auto it = interaction_handlers_.find(machine_id);
    if (it == interaction_handlers_.end()) {
        spdlog::warn("No interaction handler for machine_id={}", machine_id);
        return;
    }
    if (!it->second(x, y, z, player_id)) {
        spdlog::debug("Interaction handler returned false for machine_id={}", machine_id);
    }
}

bool SimulationEngine::tryActivateRotareGenerator(int32_t x, int32_t y, int32_t z) {
    auto ent = findEntityAt(static_cast<uint32_t>(x), static_cast<uint32_t>(y), static_cast<uint32_t>(z));
    if (ent == entt::null) return false;

    auto* machine = reg_.try_get<MachineComponent>(ent);
    if (!machine) return false;
    if (machine->machine_id != RotareGeneratorSystem::kRotareGeneratorBlockId) return false;

    auto* energy = reg_.try_get<EnergyStorage>(ent);
    if (!energy) return false;

    auto& state = reg_.emplace_or_replace<RotareState>(ent);
    if (state.spinning) return false;

    state.spinning = true;
    state.remainingTicks = RotareGeneratorSystem::kSpinDurationTicks;
    state.energyPerTick = RotareGeneratorSystem::kEnergyPerTick;

    spdlog::info("Rotare generator activated at ({},{},{})", x, y, z);
    return true;
}

std::vector<uint8_t> SimulationEngine::serializeMultiblock(uint64_t controller_id) const
{
    auto it = controllers_.find(controller_id);
    if (it == controllers_.end()) return {};

    const auto& ctrl = it->second;
    int32_t heat_stored = 0;
    int32_t recipe_progress = 0;
    int32_t recipe_ticks = 0;
    std::string recipe_id;

    auto entity = findEntityAt(ctrl.x, ctrl.y, ctrl.z);
    if (entity != entt::null) {
        if (auto* heat = reg_.try_get<HeatIntakeComponent>(entity)) {
            heat_stored = heat->heat_stored;
        }
        if (auto* prog = reg_.try_get<RecipeProgress>(entity)) {
            recipe_progress = static_cast<int32_t>(prog->remaining_ticks);
            recipe_ticks = static_cast<int32_t>(prog->remaining_ticks);
            recipe_id = prog->recipe_id;
        }
    }

    // Persist hatch/controller inventory contents (task 2.2) so nothing is
    // lost on chunk unload or world-initiated dissociation.
    std::vector<Protocol::ItemStack> slots;
    if (entity != entt::null) {
        if (auto* container = reg_.try_get<InventoryContainer>(entity)) {
            slots.reserve(container->slots.size());
            for (const auto& s : container->slots) {
                slots.emplace_back(s.item_id, s.count, s.meta);
            }
        }
    }

    flatbuffers::FlatBufferBuilder builder(256);
    auto recipe_off = builder.CreateString(recipe_id);
    auto blocks_off = builder.CreateVector(ctrl.blocks);
    auto slots_off = slots.empty() ? 0 : builder.CreateVectorOfStructs(slots);
    auto state = Protocol::CreateMultiblockState(
        builder, 1, ctrl.id, 0, ctrl.x, ctrl.y, ctrl.z, ctrl.pattern_id,
        blocks_off, heat_stored, recipe_progress, recipe_ticks, recipe_off,
        slots_off);
    builder.Finish(state);

    return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

void SimulationEngine::deserializeMultiblock(uint64_t controller_id,
                                             const uint8_t* data, size_t size)
{
    if (!data || size == 0) return;
    flatbuffers::Verifier verifier(data, size);
    if (!verifier.VerifyBuffer<Protocol::MultiblockState>(nullptr)) return;
    auto fb = flatbuffers::GetRoot<Protocol::MultiblockState>(data);

    auto it = controllers_.find(controller_id);
    if (it == controllers_.end()) return;
    auto& ctrl = it->second;

    auto entity = findEntityAt(ctrl.x, ctrl.y, ctrl.z);
    if (entity == entt::null) return;

    if (auto* heat = reg_.try_get<HeatIntakeComponent>(entity)) {
        heat->heat_stored = fb->heat_stored();
    }
    if (auto* prog = reg_.try_get<RecipeProgress>(entity)) {
        prog->recipe_id = fb->recipe_id() ? fb->recipe_id()->str() : "";
        prog->remaining_ticks = static_cast<uint32_t>(fb->recipe_ticks());
        prog->is_processing = !prog->recipe_id.empty();
    }
    if (auto* inv = reg_.try_get<InventoryContainer>(entity)) {
        if (fb->slots()) {
            inv->slots.clear();
            inv->slots.reserve(fb->slots()->size());
            for (flatbuffers::uoffset_t i = 0; i < fb->slots()->size(); ++i) {
                auto* s = fb->slots()->Get(i);
                inv->slots.emplace_back(static_cast<uint16_t>(s->item_id()),
                                        static_cast<uint8_t>(s->count()),
                                        static_cast<uint16_t>(s->meta()));
            }
            inv->slot_count = static_cast<uint16_t>(inv->slots.size());
        }
    }
    spdlog::info("[ECS] Restored multiblock #{} state at ({},{},{})",
                 controller_id, ctrl.x, ctrl.y, ctrl.z);
}

} // namespace simcore