#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

/**
 * Entity State Storage Interface
 * Handles world-bound entity state: machines, workbenches, chests, etc.
 */
class IEntityStateStorage {
public:
    virtual ~IEntityStateStorage() = default;

    // Entity State Operations
    virtual bool LoadEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z, 
                                uint16_t entityType, std::vector<uint8_t>& stateData) = 0;
    virtual bool SaveEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z, 
                                uint16_t entityType, const std::vector<uint8_t>& stateData) = 0;
    virtual bool DeleteEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z, 
                                  uint16_t entityType) = 0;
};

