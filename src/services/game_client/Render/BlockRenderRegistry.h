#pragma once
#include "PipeMeshBuilder.h" // for PipeType enum
#include <cstdint>

#include "common/ItemId.h"

inline bool isPipeBlock(uint16_t blockId) {
  return ItemId::isPipe(blockId);
}

inline bool isCableBlock(uint16_t blockId) {
  return ItemId::isCable(blockId);
}

// Machines live in the CAT_MACHINES range (1110:*). A pipe/cable adjacent to a
// machine renders a connection flange even though the neighbour is not a
// same-type pipe/cable block.
inline bool isMachineBlock(uint16_t blockId) {
  return ItemId::category(blockId) == ItemId::CAT_MACHINES;
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