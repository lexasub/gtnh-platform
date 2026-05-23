#pragma once
#include <cstdint>
#include <vector>

namespace simcore {

struct InventorySlot
{
    uint16_t item_id = 0;
    uint8_t  count = 0;
    uint16_t meta = 0;

    InventorySlot() = default;
    InventorySlot(uint16_t id, uint8_t count, uint16_t meta)
        : item_id(id), count(count), meta(meta) {}
};

struct InventoryContainer
{
    uint16_t entity_type = 0;  // 0=chest, 1=furnace, 2=electrolyser
    uint16_t slot_count = 0;
    std::vector<InventorySlot> slots;

    InventoryContainer() = default;
    InventoryContainer(uint16_t type, uint16_t count, const std::vector<InventorySlot>& items)
        : entity_type(type), slot_count(count), slots(items) {}

    InventorySlot getSlot(uint16_t index) const
    {
        return (index < slots.size()) ? slots[index] : InventorySlot{};
    }

    void setSlot(uint16_t index, const InventorySlot& slot)
    {
        if (index >= slots.size())
            slots.resize(index + 1);
        slots[index] = slot;
    }

    int32_t addItem(uint16_t item_id, uint8_t count, uint16_t meta)
    {
        for (auto& slot : slots) {
            if (slot.item_id == item_id && slot.count < 64) {
                uint8_t added = (slot.count + count) - 64;
                slot.count += count;
                if (slot.count > 64) {
                    slot.count = 64;
                    slot.meta = meta;
                }
                return (int32_t)added;
            }
        }

        for (auto& slot : slots) {
            if (slot.item_id == 0) {
                slot.item_id = item_id;
                slot.count = (count > 64) ? 64 : count;
                slot.meta = meta;
                return 0;
            }
        }

        return (int32_t)count;
    }

    bool removeItem(uint16_t index, uint8_t count)
    {
        if (index >= slots.size())
            return false;
        auto& slot = slots[index];
        if (count > slot.count)
            return false;

        slot.count -= count;
        if (slot.count == 0) {
            slots.erase(slots.begin() + index);
        }
        return true;
    }
};

} // namespace simcore
