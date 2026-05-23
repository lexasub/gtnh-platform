#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <flatbuffers/flatbuffers.h>

// Forward declarations
namespace Protocol { class InventorySlot; }

/**
 * Player Inventory Storage Interface
 * Handles player-bound data: inventories and positions
 */
class IPlayerInventoryStorage {
public:
    virtual ~IPlayerInventoryStorage() = default;

    // Player Inventory Operations
    virtual bool LoadPlayerInventory(uint64_t playerId, std::vector<uint8_t>& inventoryData) = 0;
    virtual bool SavePlayerInventory(uint64_t playerId, const std::vector<uint8_t>& inventoryData) = 0;
    virtual bool GetPlayerPosition(uint64_t playerId, int32_t& x, int32_t& y, int32_t& z) = 0;
    virtual bool SavePlayerPosition(uint64_t playerId, int32_t x, int32_t y, int32_t z) = 0;
};

