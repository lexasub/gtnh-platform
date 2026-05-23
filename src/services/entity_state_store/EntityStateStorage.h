#pragma once

#include "../storage_interfaces/IEntityStateStorage.h"
#include "cache.h"
#include <lmdb.h>
#include <asio/io_context.hpp>
#include <flatbuffers/flatbuffer_builder.h>
#include "Client/MessageRouterClient.h"
#include "Client/ChunkStoreClient.h"
#include <memory>

/**
 * LMDB Implementation of Entity State Storage
 * Handles world-bound entity state (machines, workbenches, chests, etc.)
 * via LMDB with caching layer
 */
class EntityStateStorage : public IEntityStateStorage {
public:
    EntityStateStorage(const std::string& lmdbPath, asio::io_context& io_context, size_t cacheSize = 1000);
    ~EntityStateStorage() override;

    bool initialize();
    void shutdown();

    // IEntityStateStorage
    bool LoadEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z,
                         uint16_t entityType, std::vector<uint8_t>& stateData) override;

    bool SaveEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z,
                        uint16_t entityType, const std::vector<uint8_t>& stateData) override;

    bool DeleteEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z,
                          uint16_t entityType) override;

private:
    std::string makeKey(int32_t dimension, int32_t x, int32_t y, int32_t z, uint16_t entityType);

    bool doLoadEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z,
                           uint16_t entityType, std::vector<uint8_t>& stateData);
    bool doSaveEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z,
                           uint16_t entityType, const std::vector<uint8_t>& stateData);
    bool doDeleteEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z,
                             uint16_t entityType);

    std::string lmdbPath_;
    MDB_env* env_ = nullptr;
    gtnh::entity_state_store::Cache cache_;
    gtnh::entity_state_store::MessageRouterClient messageRouterClient_;
    gtnh::entity_state_store::ChunkStoreClient chunkStoreClient_;
    asio::io_context& ioContext_;
};

