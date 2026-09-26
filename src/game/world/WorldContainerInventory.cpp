#include "WorldContainerInventory.h"
#include <apps/simcore/Network/clients/EntityStateStoreClient.h>
#include "machine_state_generated.h"
#include "entity_state_store_generated.h"
#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <unordered_map>

namespace simcore {

// Matches InventoryClick.h's kMaxStack and the wire comment on
// core.fbs:268 ("stack size (1-64)").
static constexpr uint8_t kMaxStackSize = 64;

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
    // gp-x91u: the old key was (x << 42) | (y << 21) | z, three 21-bit fields
    // with no bias and no range check. A coordinate at or past 2^21 spilled into
    // its neighbour and the key of a DIFFERENT position, so a container at the
    // far end of a world was silently swallowed as a duplicate of another one
    // — unreachable, and released by the wrong close. (The ±30M coordinates a
    // modded overworld uses are well past 2^21 = 2M, so this was reachable in
    // normal play, not just at the type's edge.)
    //
    // Each axis now takes a full 32 bits of its own, so the key is injective
    // over the whole int32 coordinate range. packKey takes uint32_t parameters,
    // and callers cast from a negative int32, so negative coordinates arrive as
    // two's-complement and stay distinct (the old key needed a 21-bit bias to
    // do that, and had none).
    return (static_cast<uint64_t>(x) << 32)
         | (static_cast<uint64_t>(y) << 16)
         | static_cast<uint64_t>(static_cast<uint16_t>(z));
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

    if (action == 0) {  // MOVE
        auto src = container.getSlot(src_slot);
        if (src.item_id == 0) return;

        // gp-x91u: `count` used to be discarded outright. count == 0 keeps the
        // old "whole stack" meaning (what a plain left-click drag sends); a
        // non-zero count is the exact amount to move — which is what the client
        // sends for a right-click "place one" (DragManager.cpp:107) and for a
        // partial merge (:157). Clamped to what the source actually holds, so
        // an over-large count can never invent items.
        uint8_t moved = count == 0 ? src.count : std::min(count, src.count);
        if (moved == 0) return;

        auto dst = container.getSlot(dst_slot);

        if (dst.item_id == 0) {
            container.setSlot(dst_slot, InventorySlot(src.item_id, moved, src.meta));
            drainSlot(container, src_slot, moved);
        } else if (dst.item_id == src.item_id && dst.meta == src.meta &&
                   dst.count < kMaxStackSize) {
            uint8_t space = kMaxStackSize - dst.count;
            uint8_t n = std::min(space, moved);
            dst.count += n;
            container.setSlot(dst_slot, dst);
            drainSlot(container, src_slot, n);
        } else {
            container.setSlot(src_slot, dst);
            container.setSlot(dst_slot, src);
        }

        spdlog::debug("[WorldContainerInventory] MOVE src={} dst={} n={} in ({},{},{})",
                      src_slot, dst_slot, moved, x, y, z);

    } else if (action == 1) {  // SPLIT
        auto src = container.getSlot(src_slot);
        if (src.item_id == 0) return;

        // count == 0 keeps the "half the stack" meaning a right-click pick-up
        // sends (DragManager.cpp:35-48); an explicit count wins.
        uint8_t half = count == 0 ? static_cast<uint8_t>((src.count + 1) / 2)
                                  : std::min(count, src.count);
        if (half == 0) return;

        auto dst = container.getSlot(dst_slot);

        if (dst.item_id == 0) {
            container.setSlot(dst_slot,
                InventorySlot(src.item_id, half, src.meta));
            drainSlot(container, src_slot, half);
        } else if (dst.item_id == src.item_id && dst.meta == src.meta &&
                   dst.count < kMaxStackSize) {
            uint8_t space = kMaxStackSize - dst.count;
            uint8_t n = std::min(half, space);
            dst.count += n;
            container.setSlot(dst_slot, dst);
            drainSlot(container, src_slot, n);
        } else {
            // gp-x91u: this branch did not exist, so a SPLIT onto an
            // incompatible destination (a different item, or a full stack of
            // the same item) fell through and did NOTHING — while the client,
            // which has already decremented its own slot optimistically,
            // believed the half had moved. The half was lost. The destination
            // cannot take a partial, so the two stacks exchange places, which
            // is exactly what MOVE already does for the same destination and
            // keeps the outcome consistent between the two actions.
            container.setSlot(src_slot, dst);
            container.setSlot(dst_slot, src);
        }

        spdlog::debug("[WorldContainerInventory] SPLIT src={} dst={} half={} in ({},{},{})",
                      src_slot, dst_slot, half, x, y, z);

    } else {
        spdlog::warn("[WorldContainerInventory] Unknown action {}", action);
    }

    // gp-x91u: slot_count drifted from slots.size() because setSlot resizes
    // the vector while nothing revisited slot_count — and the inconsistent
    // pair is what got serialised to entitystated. Keep them in lockstep.
    container.slot_count = static_cast<uint16_t>(container.slots.size());
}

void WorldContainerInventory::drainSlot(
    InventoryContainer& container, uint16_t index, uint8_t count)
{
    // InventoryContainer::removeItem ERASES the slot once the count reaches
    // zero (InventoryContainer.h:72-74), which slid every later slot down by
    // one: the item the client had placed in slot N ended up in slot N-1, and
    // the whole container's slot identity shifted on the next save/reload.
    // A container slot index is a wire address, so a drain must write an EMPTY
    // slot in place and never remove one.
    auto slot = container.getSlot(index);
    if (slot.count <= count) {
        container.setSlot(index, InventorySlot{});
        return;
    }
    slot.count -= count;
    container.setSlot(index, slot);
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
    if (!storage_) {
        // gp-j2gg: the old code returned here, which released NOTHING — the
        // entity and the open_containers_ entry stayed put for the process
        // lifetime, so every later open at that position was swallowed by the
        // "already open" guard and the position was permanently wedged. With
        // no client there is nothing to persist and no way ever to persist it,
        // so the close completes and releases; the contents are simply not
        // saved, which is the honest outcome of having no storage at all.
        spdlog::warn("[WorldContainerInventory] Close: no storage client for "
                     "({},{},{}) — releasing without saving", x, y, z);
        releaseContainer(std::move(it));
        return;
    }

    auto& container = reg_.get<InventoryContainer>(it->second.entity);
    auto blob = serializeToBlob(container);
    storage_->SaveEntityState(0,
                                   static_cast<int32_t>(x),
                                   static_cast<int32_t>(y),
                                   static_cast<int32_t>(z),
                                   container.entity_type,
                                   blob, [x, y, z, this, it_ = std::move(it)] (bool res) {
                                       // gp-j2gg: the release used to be
                                       // unconditional, so a failed write
                                       // destroyed the only copy of the
                                       // container's contents — a transient
                                       // entitystated outage silently ate
                                       // whatever the player had put in. A
                                       // failed save now KEEPS the container
                                       // open and registered, so the player
                                       // still has their items and closing
                                       // again retries the write.
                                       if (!res) {
                                           spdlog::error("[WorldContainerInventory] Failed to save container "
                                                         "({},{},{}) — keeping it open so the close can be retried",
                                                         x, y, z);
                                           return;
                                       }

                                       releaseContainer(std::move(it_));

                                       spdlog::debug("[WorldContainerInventory] Closed container ({},{},{})", x, y, z);
                                   });
}

void WorldContainerInventory::releaseContainer(
    std::unordered_map<uint64_t, OpenContainer>::iterator it)
{
    // The save callback captures the iterator — it is the only handle on the
    // open_containers_ entry — so the release has to go through it. Callers
    // hand their own iterator over with std::move.
    if (it == open_containers_.end()) return;
    reg_.destroy(it->second.entity);
    open_containers_.erase(it);
}

void WorldContainerInventory::loadContainer(
    uint32_t x, uint32_t y, uint32_t z,
    InventoryContainer container, uint64_t player_id, uint64_t key)
{
    if (!storage_) {
        spdlog::warn("[WorldContainerInventory] Open: no storage client for "
                     "({},{},{}) — opening an empty, unsaved container", x, y, z);
        openContainer(x, y, z, std::move(container), player_id, key);
        return;
    }

    storage_->LoadEntityState(0,
                                   static_cast<int32_t>(x),
                                   static_cast<int32_t>(y),
                                   static_cast<int32_t>(z),
                                   container.entity_type,
                                   [x, y, z, cont = std::move(container), this, key, player_id] (const EntityStateStoreClient::EntityStateData& stat) {
                                       // An EMPTY state is the normal reply for a
                                       // position that has never been saved — every
                                       // freshly placed chest. It is also exactly
                                       // what EntityStateStoreClient hands back
                                       // when it is not connected
                                       // (EntityStateStoreClient.cpp:54), so the
                                       // two cases cannot be told apart here.
                                       // Either way the container is opened EMPTY,
                                       // sized by the entity type: refusing to open
                                       // would leave a new chest permanently
                                       // unopenable. Matches ChestOpenHandler.cpp:32-40
                                       // and MachineOpenHandler.cpp:78-92, which
                                       // both register the session first and
                                       // hydrate it on the load callback
                                       // ("session must exist before any click
                                       // arrives").
                                       InventoryContainer opened = std::move(cont);
                                       if (!stat.state.empty()) {
                                           spdlog::debug("[WorldContainerInventory] Restoring saved state for "
                                                         "({},{},{})", x, y, z);
                                           deserializeFromBlob(stat.state, opened);
                                       } else {
                                           spdlog::debug("[WorldContainerInventory] No saved state for "
                                                         "({},{},{}) — using empty", x, y, z);
                                       }

                                       openContainer(x, y, z, std::move(opened), player_id, key);
                                   });

}

void WorldContainerInventory::openContainer(
    uint32_t x, uint32_t y, uint32_t z,
    InventoryContainer container, uint64_t player_id, uint64_t key)
{
    const uint16_t entity_type = container.entity_type;
    const uint16_t slot_count = container.slot_count;
    auto entity = reg_.create();
    reg_.emplace<InventoryContainer>(entity, std::move(container));
    open_containers_[key] = {entity, player_id};

    spdlog::debug("[WorldContainerInventory] Opened container ({},{},{}) type={} slots={}",
                  x, y, z, entity_type, slot_count);
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
