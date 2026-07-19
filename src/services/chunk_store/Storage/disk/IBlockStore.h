#pragma once
#include <common/coords/Coords.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class IBlockStore {
public:
    virtual ~IBlockStore() = default;

    virtual bool HasChunk(ChunkCoord c) const = 0;

    virtual bool writeRaw(int64_t key, const uint8_t* data, size_t size) = 0;

    virtual bool writeBatch(
        std::vector<std::pair<int64_t, std::shared_ptr<std::vector<uint8_t>>>>& items) = 0;

    virtual std::optional<std::vector<uint8_t>> readRawBytes(int64_t key) const = 0;
};
