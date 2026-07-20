#pragma once
#include "PipeMeshBuilder.h" // for PipeType enum
#include <cstdint>

#include "common/ItemId.h"

// Pipe block IDs: fluid=61, item=62, dense_item=64, dense_fluid=65
// Cable block IDs: tin=80, copper=81, gold=82, alu=83, tungsten=84, platinum=85

inline bool isPipeBlock(uint16_t blockId) {
  //TODO - may be bit check
  return blockId >= ItemId::pack("1111:10:0") && blockId < ItemId::pack("1111:11:0");
}

inline bool isCableBlock(uint16_t blockId) {
  return blockId >= ItemId::pack("1111:01:0") && blockId < ItemId::pack("1111:10:0");
}

inline PipeType blockIdToPipeType(uint16_t blockId) {
  return static_cast<PipeType>(
    (blockId - ItemId::pack("1111:10:0"))
    % (static_cast<int>(PipeType::CABLE_PLATINUM) + 1));
}

// Returns cable tier (1-4). 0 = not a cable block.
inline uint8_t blockIdToCableTier(uint16_t blockId) {
    if (blockId >= ItemId::pack("1111:01:0") && blockId <= ItemId::pack("1111:01:5"))
        return static_cast<uint8_t>(blockId - ItemId::pack("1111:01:0") + 1);
    return 0;
}