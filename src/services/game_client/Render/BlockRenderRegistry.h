#pragma once
#include "PipeMeshBuilder.h" // for PipeType enum
#include <cstdint>

// Pipe block IDs: fluid=61, item=62, dense_item=64, dense_fluid=65
// Cable block IDs: tin=80, copper=81, gold=82, alu=83, tungsten=84, platinum=85

inline bool isPipeBlock(uint16_t blockId) {
  return blockId == 61 || blockId == 62 || blockId == 64 || blockId == 65;
}

inline bool isCableBlock(uint16_t blockId) {
  return blockId >= 80 && blockId <= 85;
}

inline PipeType blockIdToPipeType(uint16_t blockId) {
  switch (blockId) {
  case 61:
    return PipeType::FLUID_PIPE;
  case 62:
    return PipeType::ITEM_PIPE;
  case 64:
    return PipeType::DENSE_ITEM_PIPE;
  case 65:
    return PipeType::DENSE_FLUID_PIPE;
  default:
    return PipeType::ITEM_PIPE; // fallback
  }
}

// Returns cable tier (1-4). 0 = not a cable block.
inline uint8_t blockIdToCableTier(uint16_t blockId) {
  if (blockId >= 80 && blockId <= 85)
    return static_cast<uint8_t>(blockId - 79); // 80→1, 81→2, ..., 85→6
  return 0;
}