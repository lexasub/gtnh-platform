#pragma once

#include "Common/Types.h"

// Pure interface for block querying (used by Raycaster and mesh builder)
class IBlockQuery {
public:
    virtual ~IBlockQuery() = default;
    // Returns block ID (0 = air)
    virtual uint16_t GetBlockAt(BlockPos pos) const = 0;
};