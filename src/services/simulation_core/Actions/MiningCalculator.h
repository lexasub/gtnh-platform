#pragma once
#include <cstdint>
#include <cmath>
#include <common/ItemId.h>
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

// Block hardness definitions
constexpr uint8_t getBlockHardness(uint16_t block_id) {
    if (block_id == ItemId::pack("0:0:4")) return 0;  // glass
    if (block_id == ItemId::pack("0:10:0")) return 1;  // oak_planks
    if (block_id == ItemId::pack("0:0:1")) return 2;   // stone
    if (block_id == ItemId::pack("0:0:2")) return 2;   // cobblestone
    if (block_id == ItemId::pack("0:0:5")) return 3;   // obsidian
    if (ItemId::category(block_id) == ItemId::CAT_ORES) return 3;
    return 1;  // default
}

// Mining level required to break a block
constexpr uint8_t getBlockMiningLevel(uint16_t block_id) {
    if (block_id == ItemId::pack("10:9")) return 3;  // diamond_ore — needs HV
    if (block_id == ItemId::pack("10:8")) return 2;  // lapis_ore — needs MV
    if (block_id == ItemId::pack("10:7")) return 2;  // redstone_ore — needs MV
    if (block_id == ItemId::pack("10:1")) return 2;  // gold_ore — needs MV
    return 1;  // most ores — needs LV
}

constexpr bool logIsWoodBlock(uint16_t block_id) {
    // All wood items are under prefix "010" (wood category)
    // Check top 3 bits = 010 → range 0x4000–0x5FFF (16384–24575)
    return (block_id & 0xE000) == 0x4000;
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
