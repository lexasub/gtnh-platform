#pragma once
#include <common/ItemId.h>
#include <cstdint>

// Tool item IDs — computed at compile time from prefix notation.
// Devices with special mining behavior.

constexpr uint16_t ITEM_DRILL_ULV = ItemId::pack("1111:00:0");
constexpr uint16_t ITEM_DRILL_LV = ItemId::pack("1111:00:1");
constexpr uint16_t ITEM_DRILL_MV = ItemId::pack("1111:00:2");
constexpr uint16_t ITEM_DRILL_HV = ItemId::pack("1111:00:3");
constexpr uint16_t ITEM_CHAINSAW_LV = ItemId::pack("1111:00:4");
constexpr uint16_t ITEM_WRENCH = ItemId::pack("1111:00:5");

// Battery buffers & charger — under MACHINES category
constexpr uint16_t BLOCK_BATTERY_BUFFER_LV = ItemId::pack("1110:10:0");
constexpr uint16_t BLOCK_BATTERY_BUFFER_MV = ItemId::pack("1110:10:1");
constexpr uint16_t BLOCK_BATTERY_BUFFER_HV = ItemId::pack("1110:10:2");
constexpr uint16_t BLOCK_CHARGER = ItemId::pack("1110:10:3");

// Rotare machines — under MACHINES category
constexpr uint16_t BLOCK_ROTARE_GENERATOR = ItemId::pack("1110:01:3");
constexpr uint16_t BLOCK_ROTARE_MACERATOR = ItemId::pack("1110:00:8");

// Tool tier from item ID — uses payload encoding under 1111:00: prefix
// Payload: [tier:5][type:6] — tier = (payload >> 6) & 0x1F
constexpr uint8_t toolTier(uint16_t itemId) {
  return static_cast<uint8_t>(ItemId::toolTier(itemId));
}

// Mining level from tier index
constexpr uint8_t miningLevel(uint8_t tier) {
  constexpr uint8_t levels[] = {1, 2, 3, 4};
  return (tier < 4) ? levels[tier] : 4;
}
