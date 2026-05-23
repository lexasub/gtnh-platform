#pragma once
#include <cstdint>

constexpr uint16_t ITEM_DRILL_ULV    = 90;
constexpr uint16_t ITEM_DRILL_LV     = 91;
constexpr uint16_t ITEM_DRILL_MV     = 92;
constexpr uint16_t ITEM_DRILL_HV     = 93;
constexpr uint16_t ITEM_CHAINSAW_LV  = 94;
constexpr uint16_t ITEM_WRENCH       = 95;

constexpr uint16_t BLOCK_BATTERY_BUFFER_LV = 96;
constexpr uint16_t BLOCK_BATTERY_BUFFER_MV = 97;
constexpr uint16_t BLOCK_BATTERY_BUFFER_HV = 98;
constexpr uint16_t BLOCK_CHARGER           = 99;

constexpr uint8_t toolTier(uint16_t itemId) {
    switch (itemId) {
        case ITEM_DRILL_ULV: return 0;
        case ITEM_DRILL_LV:
        case ITEM_CHAINSAW_LV: return 1;
        case ITEM_DRILL_MV: return 2;
        case ITEM_DRILL_HV: return 3;
        default: return 0;
    }
}

constexpr uint8_t miningLevel(uint8_t tier) {
    constexpr uint8_t levels[] = {1, 2, 3, 4};
    return (tier < 4) ? levels[tier] : 4;
}