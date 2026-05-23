#pragma once
#include <cstdint>
#include <vector>
#include "../ECS/components/InventoryContainer.h"

namespace simcore {

/// Pack inventory slots to 5‑bytes‑per‑slot wire format for BlockEntityUpdate.
/// Layout per slot: item_id[2] + count[1] + meta[2] (little‑endian).
inline std::vector<uint8_t> packInventory(const std::vector<InventorySlot>& slots) {
    std::vector<uint8_t> data;
    data.reserve(slots.size() * 5);
    for (const auto& slot : slots) {
        auto ptr = reinterpret_cast<const uint8_t*>(&slot.item_id);
        data.push_back(ptr[0]); data.push_back(ptr[1]);
        data.push_back(slot.count);
        ptr = reinterpret_cast<const uint8_t*>(&slot.meta);
        data.push_back(ptr[0]); data.push_back(ptr[1]);
    }
    return data;
}

} // namespace simcore
