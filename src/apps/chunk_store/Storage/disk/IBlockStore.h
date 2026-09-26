#pragma once
#include <engine/registry/coords/Coords.h>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

// gp-wjmb: why a read produced no bytes. "Not stored" and "could not read" are
// different facts and must not share one value — a caller that cannot tell them
// apart treats a disk error as an empty chunk and writes over real terrain.
enum class ReadError {
    None,      // a value was read successfully
    NotFound,  // the key is genuinely absent from the database
    Failed,    // the read could not be performed (I/O, txn, reader table, …)
};

class IBlockStore {
public:
    virtual ~IBlockStore() = default;

    virtual bool HasChunk(ChunkCoord c) const = 0;

    virtual bool writeRaw(int64_t key, const uint8_t* data, size_t size) = 0;

    virtual bool writeBatch(
        std::vector<std::pair<int64_t, std::shared_ptr<std::vector<uint8_t>>>>& items) = 0;

    // gp-wjmb: the error slot is what makes a read failure distinguishable from
    // an absent chunk. Callers MUST check it before treating a missing value as
    // "no chunk stored".
    virtual std::expected<std::vector<uint8_t>, ReadError> readRawBytes(
        int64_t key) const = 0;
};
