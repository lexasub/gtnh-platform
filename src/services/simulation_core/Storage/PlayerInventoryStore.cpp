#include "Storage/PlayerInventoryStore.h"

#include <spdlog/spdlog.h>

namespace simcore {

PlayerInventoryStore::PlayerInventoryStore() = default;

std::array<PersistSlot, kInventorySlots> PlayerInventoryStore::getSlots(uint64_t player_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = inventories_.find(player_id);
    if (it != inventories_.end()) return it->second;
    return {};
}

void PlayerInventoryStore::setSlots(uint64_t player_id,
                                     const std::array<PersistSlot, kInventorySlots>& slots) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inventories_[player_id] = slots;
    }
    if (onChange_) {
        for (uint16_t i = 0; i < kInventorySlots; ++i) {
            const auto& s = slots[i];
            onChange_(player_id, i, s.item_id, s.count, s.meta);
        }
    }
    if (postMutation_) {
        postMutation_(player_id, slots);
    }
}

PersistSlot PlayerInventoryStore::getCursor(uint64_t player_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cursors_.find(player_id);
    return it != cursors_.end() ? it->second : PersistSlot{};
}

void PlayerInventoryStore::setCursor(uint64_t player_id, const PersistSlot& cursor) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cursors_[player_id] = cursor;
    }
    if (postMutation_) {
        postMutation_(player_id, getSlots(player_id));
    }
}

void PlayerInventoryStore::setSlotsAndCursor(
    uint64_t player_id,
    const std::array<PersistSlot, kInventorySlots>& slots,
    const PersistSlot& cursor) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inventories_[player_id] = slots;
        cursors_[player_id] = cursor;
    }
    if (onChange_) {
        for (uint16_t i = 0; i < kInventorySlots; ++i) {
            const auto& s = slots[i];
            onChange_(player_id, i, s.item_id, s.count, s.meta);
        }
    }
    if (postMutation_) {
        postMutation_(player_id, slots);
    }
}

void PlayerInventoryStore::initPlayer(uint64_t player_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    inventories_.try_emplace(player_id);
}

void PlayerInventoryStore::applyUpdate(uint64_t player_id, const std::vector<PersistSlot>& slots) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& target = inventories_[player_id];
    for (size_t i = 0; i < slots.size() && i < kInventorySlots; ++i) {
        target[i] = slots[i];
    }
}

bool PlayerInventoryStore::giveItem(uint64_t player_id, uint16_t item_id,
                                     uint8_t count, int32_t target_slot) {
    constexpr uint8_t kMaxStack = 64;
    int remaining = static_cast<int>(count);

    spdlog::info("giveItem: player={} item={} count={} target_slot={}", player_id, item_id, count, target_slot);

    std::unique_lock<std::mutex> lock(mutex_);
    auto& slots = inventories_[player_id];

    if (target_slot >= 0 && target_slot < kInventorySlots) {
        auto& dst = slots[target_slot];
        if (dst.item_id == 0) {
            dst = {item_id, static_cast<uint8_t>(remaining), 0};
            remaining = 0;
        } else if (dst.item_id == item_id && dst.count < kMaxStack) {
            uint8_t room = kMaxStack - dst.count;
            uint8_t add = std::min(static_cast<uint8_t>(remaining), room);
            dst.count += add;
            remaining -= add;
        }
    }

    if (remaining > 0) {
        for (auto& s : slots) {
            if (remaining <= 0) break;
            if (s.item_id == item_id && s.count < kMaxStack) {
                uint8_t room = kMaxStack - s.count;
                uint8_t add = std::min(static_cast<uint8_t>(remaining), room);
                s.count += add;
                remaining -= add;
            }
        }
    }
    if (remaining > 0) {
        for (auto& s : slots) {
            if (remaining <= 0) break;
            if (s.item_id == 0) {
                uint8_t add = std::min(static_cast<uint8_t>(remaining), kMaxStack);
                s = {item_id, add, 0};
                remaining -= add;
            }
        }
    }

    if (remaining > 0) {
        spdlog::warn("Inventory full, dropping {} of item {}", remaining, item_id);
    }

    PersistSlot snapshot[kInventorySlots];
    for (uint16_t i = 0; i < kInventorySlots; ++i) {
        snapshot[i] = slots[i];
    }

    if (onChange_) {
        for (uint16_t i = 0; i < kInventorySlots; ++i) {
            const auto& s = slots[i];
            onChange_(player_id, i, s.item_id, s.count, s.meta);
        }
    }

    lock.unlock();

    if (postMutation_) {
        std::array<PersistSlot, kInventorySlots> arr;
        for (uint16_t i = 0; i < kInventorySlots; ++i) arr[i] = snapshot[i];
        postMutation_(player_id, arr);
    }

    return remaining == 0;
}

flatbuffers::Offset<Protocol::InventoryUpdate> PlayerInventoryStore::buildUpdate(
    flatbuffers::FlatBufferBuilder& builder, uint64_t player_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = inventories_.find(player_id);
    std::vector<flatbuffers::Offset<Protocol::InventorySlot>> fbSlots;
    if (it != inventories_.end()) {
        fbSlots.reserve(kInventorySlots);
        for (const auto& s : it->second) {
            fbSlots.push_back(Protocol::CreateInventorySlot(builder, s.item_id, s.count, s.meta));
        }
    }
    auto slotsVec = builder.CreateVector(fbSlots);

    // Server-owned cursor stack.
    auto cit = cursors_.find(player_id);
    Protocol::ItemStack cursorStruct(cit != cursors_.end() ? cit->second.item_id : 0,
                                     cit != cursors_.end() ? cit->second.count : 0,
                                     cit != cursors_.end() ? cit->second.meta : 0);

    // Open-container fields — filled by the container session (Phase B+);
    // empty by default so player-only snapshots are complete.
    Protocol::Vec3i noPos(0, 0, 0);
    auto emptyVec = builder.CreateVector(std::vector<flatbuffers::Offset<Protocol::InventorySlot>>{});

    return Protocol::CreateInventoryUpdate(builder, player_id, slotsVec, &cursorStruct,
                                           0 /* container_id */, &noPos, emptyVec);
}

}
