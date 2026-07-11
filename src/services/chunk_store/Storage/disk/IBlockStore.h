#pragma once
#include "../../Chunk/Chunk.h"
#include <common/coords/Coords.h>
#include <cstdint>
#include <memory>
#include <vector>

// Low-level LMDB-backed chunk persistence interface.
// Operates on serialized chunk blobs, not individual blocks.
// For block-level operations use ChunkStore facade.
class IBlockStore {
public:
    virtual ~IBlockStore() = default;

    // Check if chunk exists in the database.
    virtual bool HasChunk(ChunkCoord c) const = 0;

    // LMDB-specific: raw keyed read with optional palette output.
    // Returns true if key found, false if not found or error.
    virtual bool readRaw(int64_t key, Chunk& out,
                         std::shared_ptr<std::vector<uint8_t>>* palette_out = nullptr) const = 0;

    // Write raw palette bytes. If txn is null, opens+commits own transaction.
    virtual bool writeRaw(int64_t key, const uint8_t* data, size_t size) = 0;

    // Batch write multiple (key, palette) pairs in a single transaction.
    // Opens and commits the transaction internally.
    virtual bool writeBatch(
        std::vector<std::pair<int64_t, std::shared_ptr<std::vector<uint8_t>>>>& items) = 0;
};
