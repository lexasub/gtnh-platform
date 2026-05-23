#pragma once
#include <cstdint>
#include <cmath>
#include "../ECS/components/ItemEnergyStorage.h"

// Base mining time in ticks for a player with no tool
constexpr float BASE_MINE_TIME = 20.0f;  // 1 second at 20 Hz

// Tool speed multipliers per tier
constexpr float TOOL_SPEED[] = {
    1.5f,   // tier 0 (ULV) — 1.5x hand speed
    3.0f,   // tier 1 (LV)  — 3x hand speed
    5.0f,   // tier 2 (MV)  — 5x hand speed
    8.0f,   // tier 3 (HV)  — 8x hand speed
};

// Energy cost per block hardness unit
constexpr int32_t ENERGY_PER_HARDNESS = 50;  // 50 EU per hardness level

// Block hardness definitions (hardcoded for MVP, configurable later)
constexpr uint8_t getBlockHardness(uint16_t block_id) {
    switch (block_id) {
        // Ores
        case 3:  case 5:  case 25: case 53:  // iron, gold, tin, copper
        case 70: case 71: case 72: case 73:  // coal, redstone, lapis, diamond
            return 3;
        // Stone variants
        case 1:  return 2;  // stone
        case 7:  return 2;  // cobblestone
        case 8:  return 2;  // stone (smooth)
        // Soft blocks
        case 13: return 1;  // oak_planks
        case 12: return 0;  // glass
        default:  return 1;  // default
    }
}

// Mining level required to break a block
constexpr uint8_t getBlockMiningLevel(uint16_t block_id) {
    switch (block_id) {
        case 73: return 3;  // diamond_ore — needs HV (tier 3)
        case 72: return 2;  // lapis_ore — needs MV (tier 2)
        case 71: return 2;  // redstone_ore — needs MV
        case 5:  return 2;  // gold_ore — needs MV
        default: return 1;  // most ores — needs LV (tier 1)
    }
}

constexpr bool logIsWoodBlock(uint16_t block_id) {
    switch (block_id) {
        case 13: case 14: case 15: case 16: case 17:
        case 18:
            return true;
        default: return false;
    }
}

inline int32_t miningEnergyCost(uint16_t toolItem, uint16_t blockId) {
    (void)toolItem;
    return getBlockHardness(blockId) * ENERGY_PER_HARDNESS;
}

inline float miningTicks(uint8_t toolTier, uint16_t blockId, bool isChainsaw = false) {
    float speed = (toolTier < 4) ? TOOL_SPEED[toolTier] : TOOL_SPEED[3];
    if (isChainsaw && logIsWoodBlock(blockId)) {
        speed *= 2.0f;
    }
    return BASE_MINE_TIME / speed;
}
