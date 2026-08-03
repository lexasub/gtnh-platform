#pragma once

#include "components/InventoryContainer.h"
#include "components/MultiblockController.h"
#include "../Network/IEventPublisher.h"
#include <cstdint>
#include <vector>

namespace simcore {

// Pack every inventory slot into the 5-byte wire format
// (item_id[2] + count[1] + meta[2]) used by BlockEntityUpdate.inventory.
inline std::vector<uint8_t> packInventorySlots(const InventoryContainer& container) {
    std::vector<uint8_t> out;
    out.reserve(container.slots.size() * 5);
    for (const auto& s : container.slots) {
        out.push_back(static_cast<uint8_t>(s.item_id & 0xFF));
        out.push_back(static_cast<uint8_t>((s.item_id >> 8) & 0xFF));
        out.push_back(s.count);
        out.push_back(static_cast<uint8_t>(s.meta & 0xFF));
        out.push_back(static_cast<uint8_t>((s.meta >> 8) & 0xFF));
    }
    return out;
}

// Build per-hatch update data for a multiblock controller. Each hatch carries
// its own packed slot range so the client can render contents per hatch.
inline std::vector<HatchUpdateData> buildHatchUpdateData(
    const MultiblockController& ctrl, const InventoryContainer& container) {
    std::vector<HatchUpdateData> out;
    out.reserve(ctrl.hatches.size());
    for (const auto& hs : ctrl.hatches) {
        HatchUpdateData hd;
        hd.world_x = static_cast<int32_t>(hs.world_x);
        hd.world_y = static_cast<int32_t>(hs.world_y);
        hd.world_z = static_cast<int32_t>(hs.world_z);
        hd.hatch_type = static_cast<uint8_t>(hs.type);
        hd.tier = 1;
        const int start = hs.slot_start;
        const int end = hs.slot_end;
        if (end > start) {
            hd.slot_data.reserve(static_cast<size_t>(end - start) * 5);
            for (int i = start; i < end && i < static_cast<int>(container.slots.size()); ++i) {
                const auto& s = container.slots[i];
                hd.slot_data.push_back(static_cast<uint8_t>(s.item_id & 0xFF));
                hd.slot_data.push_back(static_cast<uint8_t>((s.item_id >> 8) & 0xFF));
                hd.slot_data.push_back(s.count);
                hd.slot_data.push_back(static_cast<uint8_t>(s.meta & 0xFF));
                hd.slot_data.push_back(static_cast<uint8_t>((s.meta >> 8) & 0xFF));
            }
        }
        out.push_back(std::move(hd));
    }
    return out;
}

} // namespace simcore
