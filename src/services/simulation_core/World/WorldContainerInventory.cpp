#include "World/WorldContainerInventory.h"
#include "EntityStateStoreClient.h"
#include "machine_state_generated.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <unordered_map>

namespace simcore {

static uint16_t slotCountForType(uint16_t entity_type) {
    switch (entity_type) {
        case 1:  return 3;
        case 2:  return 9;
        default: return 27;
    }
}

WorldContainerInventory::WorldContainerInventory(
    entt::registry& reg,
    std::shared_ptr<EntityStateStoreClient> storage)
    : reg_(reg)
    , storage_(std::move(storage))
{
    if (!storage_) {
        spdlog::warn("[WorldContainerInventory] null storage — persistence disabled");
    }
}

uint64_t WorldContainerInventory::packKey(uint32_t x, uint32_t y, uint32_t z) {
    return (static_cast<uint64_t>(x) << 42)
         | (static_cast<uint64_t>(y) << 21)
         | static_cast<uint64_t>(z);
}

void WorldContainerInventory::onContainerOpen(
    uint64_t player_id,
    uint32_t x, uint32_t y, uint32_t z,
    uint16_t entity_type)
{
    auto key = packKey(x, y, z);

    auto it = open_containers_.find(key);
    if (it != open_containers_.end()) {
        spdlog::debug("[WorldContainerInventory] Container ({},{},{}) already open",
                      x, y, z);
        return;
    }

    InventoryContainer container;
    container.entity_type = entity_type;
    container.slot_count = slotCountForType(entity_type);
    container.slots.resize(container.slot_count);

    loadContainer(x, y, z, std::move(container), player_id, key);
}

void WorldContainerInventory::onContainerClose(
    uint64_t player_id,
    uint32_t x, uint32_t y, uint32_t z)
{
    auto key = packKey(x, y, z);
    auto it = open_containers_.find(key);
    if (it == open_containers_.end()) {
        spdlog::warn("[WorldContainerInventory] Close: container ({},{},{}) not open",
                     x, y, z);
        return;
    }

    if (it->second.player_id != player_id) {
        spdlog::warn("[WorldContainerInventory] Close: player {} tried to close "
                     "container opened by player {}", player_id, it->second.player_id);
        return;
    }

    saveContainer(x, y, z, std::move(it));
}

void WorldContainerInventory::onContainerAction(
    uint64_t player_id,
    uint32_t x, uint32_t y, uint32_t z,
    uint8_t action, uint8_t src_slot,
    uint8_t dst_slot, uint8_t count)
{
    (void)count;
    auto key = packKey(x, y, z);
    auto it = open_containers_.find(key);
    if (it == open_containers_.end()) {
        spdlog::warn("[WorldContainerInventory] Action: container ({},{},{}) not open",
                     x, y, z);
        return;
    }

    if (it->second.player_id != player_id) {
        spdlog::warn("[WorldContainerInventory] Action: player {} mismatch",
                     player_id);
        return;
    }

    auto& container = reg_.get<InventoryContainer>(it->second.entity);

        if (action == 0) {
            auto src = container.getSlot(src_slot);
            if (src.item_id == 0) return;

            auto dst = container.getSlot(dst_slot);

            if (dst.item_id == 0) {
                container.setSlot(dst_slot, src);
                container.removeItem(src_slot, src.count);
            } else if (dst.item_id == src.item_id && dst.count < 64) {
                uint8_t space = 64 - dst.count;
                uint8_t moved = std::min(space, src.count);
                dst.count += moved;
                container.setSlot(dst_slot, dst);
                container.removeItem(src_slot, moved);
            } else {
                container.setSlot(src_slot, dst);
                container.setSlot(dst_slot, src);
            }

        spdlog::debug("[WorldContainerInventory] MOVE src={} dst={} in ({},{},{})",
                      src_slot, dst_slot, x, y, z);

        } else if (action == 1) {
            auto src = container.getSlot(src_slot);
            if (src.item_id == 0) return;

            uint8_t half = (src.count + 1) / 2;
        auto dst = container.getSlot(dst_slot);

        if (dst.item_id == 0) {
            container.setSlot(dst_slot,
                InventorySlot(src.item_id, half, src.meta));
            container.removeItem(src_slot, half);
        } else if (dst.item_id == src.item_id && dst.count < 64) {
            uint8_t space = 64 - dst.count;
            uint8_t moved = std::min(half, space);
            dst.count += moved;
            container.setSlot(dst_slot, dst);
            container.removeItem(src_slot, moved);
        }

        spdlog::debug("[WorldContainerInventory] SPLIT src={} dst={} half={} in ({},{},{})",
                      src_slot, dst_slot, half, x, y, z);

    } else {
        spdlog::warn("[WorldContainerInventory] Unknown action {}", action);
    }
}

InventoryContainer* WorldContainerInventory::getContainer(
    uint32_t x, uint32_t y, uint32_t z)
{
    auto key = packKey(x, y, z);
    auto it = open_containers_.find(key);
    if (it == open_containers_.end()) return nullptr;
    return &reg_.get<InventoryContainer>(it->second.entity);
}

void WorldContainerInventory::saveContainer(
    uint32_t x, uint32_t y, uint32_t z,
    std::unordered_map<uint64_t, OpenContainer>::iterator it)
{
    auto& container = reg_.get<InventoryContainer>(it->second.entity);
    if (!storage_) return;

    auto blob = serializeToBlob(container);
    storage_->SaveEntityState(0,
                                   static_cast<int32_t>(x),
                                   static_cast<int32_t>(y),
                                   static_cast<int32_t>(z),
                                   container.entity_type,
                                   blob, [x, y, z, this, it_ = std::move(it)] (bool res) {
                                       if (!res)
                                       {
                                           spdlog::error("[WorldContainerInventory] Failed to save container "
                                                         "({},{},{})", x, y, z);
                                       }

                                       //iterator - Мб тут гонка возникнет - из нескольких потоков работа с глобальным изменяющимся объектом
                                       reg_.destroy(it_->second.entity);
                                       open_containers_.erase(it_);

                                       spdlog::debug("[WorldContainerInventory] Closed container ({},{},{})", x, y, z);
                                   });
}

void WorldContainerInventory::loadContainer(
    uint32_t x, uint32_t y, uint32_t z,
    InventoryContainer container, uint64_t player_id, uint64_t key)
{
    if (!storage_) return;

    storage_->LoadEntityState(0,
                                   static_cast<int32_t>(x),
                                   static_cast<int32_t>(y),
                                   static_cast<int32_t>(z),
                                   container.entity_type,
                                   [x, y, z, cont = std::move(container), this, key, player_id] (const EntityStateStoreClient::EntityStateData& stat) {


                                       if (stat.state.empty())//TODO check что не херню написали
                                       {
                                            spdlog::debug("[WorldContainerInventory] No saved state for "
                                                          "({},{},{}) — using empty", x, y, z);
                                       } else {
                                           auto container = cont;
                                           deserializeFromBlob(stat.state, container);


                                           auto entity = reg_.create();
                                           reg_.emplace<InventoryContainer>(entity, container);

                                           open_containers_[key] = {entity, player_id};

                                           spdlog::debug("[WorldContainerInventory] Opened container ({},{},{}) type={} slots={}",
                                                         x, y, z, container.entity_type, container.slot_count);
                                       }
                                   });

}

std::vector<uint8_t> WorldContainerInventory::serializeToBlob(
    const InventoryContainer& container)
{
    flatbuffers::FlatBufferBuilder fbb(256);

    std::vector<flatbuffers::Offset<Protocol::MachineInventorySlot>> slotOffsets;
    for (const auto& slot : container.slots) {
        slotOffsets.push_back(
            Protocol::CreateMachineInventorySlot(
                fbb, slot.item_id, slot.count, slot.meta));
    }

    auto machineInv = Protocol::CreateMachineInventory(
        fbb, container.slot_count, fbb.CreateVector(slotOffsets));

    auto state = Protocol::CreateMachineState(
        fbb,
        1,         // version
        0,         // energy (null)
        0,         // fluids (null)
        machineInv,
        0          // nbt_tags (null)
    );

    fbb.Finish(state);

    return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

void WorldContainerInventory::deserializeFromBlob(
    const std::vector<uint8_t>& blob,
    InventoryContainer& container)
{
    auto verifier = flatbuffers::Verifier(blob.data(), blob.size());
    if (!verifier.VerifyBuffer<Protocol::MachineState>(nullptr)) {
        spdlog::warn("[WorldContainerInventory] Invalid MachineState blob");
        return;
    }

    auto state = flatbuffers::GetRoot<Protocol::MachineState>(blob.data());

    auto* inv = state->inventory();
    if (!inv) return;

    auto* slots = inv->slots();
    if (!slots) return;

    container.slot_count = static_cast<uint16_t>(std::max(
        static_cast<int>(inv->size()),
        static_cast<int>(slots->size())));

    container.slots.clear();
    container.slots.reserve(slots->size());
    for (size_t i = 0; i < slots->size(); ++i) {
        auto* s = slots->Get(i);
        container.slots.emplace_back(s->item_id(),
                                     static_cast<uint8_t>(s->count()),
                                     s->meta());
    }
}

}
