#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include "services/storage_interfaces/IPlayerInventoryStorage.h"
#include "services/storage_interfaces/IEntityStateStorage.h"
#include "protocol/core_generated.h"
#include <flatbuffers/flatbuffers.h>

/**
 * Inventory Integration Layer
 * 
 * Routes inventory requests to the appropriate storage backend based on whether
 * the data is player-bound (MetaDB) or world-bound (EntityStateStore).
 * 
 * This implements the storage architecture specification:
 * - MetaDB: player-bound data (inventories, player positions)
 * - EntityStateStore: world-bound entity state (machines, workbenches, etc.)
 * 
 * ChunkStore is NOT part of this abstraction - it handles only block data.
 */
class InventoryIntegration {
public:
    InventoryIntegration(
        std::shared_ptr<IPlayerInventoryStorage> playerInventoryStorage,
        std::shared_ptr<IEntityStateStorage> entityStateStorage);
    ~InventoryIntegration() = default;

    // Player Inventory Operations (delegated to MetaDB)
    bool LoadPlayerInventory(uint64_t playerId, std::vector<uint8_t>& inventoryData);
    bool SavePlayerInventory(uint64_t playerId, const std::vector<uint8_t>& inventoryData);
    bool GetPlayerPosition(uint64_t playerId, int32_t& x, int32_t& y, int32_t& z);
    bool SavePlayerPosition(uint64_t playerId, int32_t x, int32_t y, int32_t z);

    // Entity State Operations (delegated to EntityStateStore)
    bool LoadEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z, 
                        uint16_t entityType, std::vector<uint8_t>& stateData);
    bool SaveEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z, 
                        uint16_t entityType, const std::vector<uint8_t>& stateData);
    bool DeleteEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z, 
                          uint16_t entityType);

    // FlatBuffers Integration Helpers
    bool ProcessInventoryUpdateRequest(const uint8_t* data, size_t len, 
                                      std::vector<uint8_t>& responseData);
    bool ProcessInventorySnapshotRequest(const uint8_t* data, size_t len,
                                        std::vector<uint8_t>& responseData);
    bool ProcessEntityStateRequest(const uint8_t* data, size_t len,
                                  std::vector<uint8_t>& responseData);

private:
    std::shared_ptr<IPlayerInventoryStorage> playerInventoryStorage_;
    std::shared_ptr<IEntityStateStorage> entityStateStorage_;

    // Helper to determine if a request is player-bound or world-bound
    bool IsPlayerBoundRequest(const uint8_t* data, size_t len);
    bool IsWorldBoundRequest(const uint8_t* data, size_t len);
};

