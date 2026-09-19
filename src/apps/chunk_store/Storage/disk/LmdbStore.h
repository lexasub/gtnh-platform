#pragma once

#include "IBlockStore.h"
#include <cstdint>
#include <lmdb.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class LmdbStore : public IBlockStore {
public:
    explicit LmdbStore(const std::string& db_path, size_t max_map_size);
    ~LmdbStore() override;

    bool HasChunk(ChunkCoord c) const override;
    bool writeRaw(int64_t key, const uint8_t* data, size_t size) override;
    bool writeBatch(
        std::vector<std::pair<int64_t, std::shared_ptr<std::vector<uint8_t>>>>& items) override;

    std::optional<std::vector<uint8_t>> readRawBytes(int64_t key) const override;

    static int64_t makeKey(int32_t cx, int32_t cy, int32_t cz);
private:
    bool growMapSize();
    void open_();
    void close_();

    MDB_env* env_ = nullptr;
    MDB_dbi dbi_ = 0;
    std::string db_path_;
    size_t maxMapSize_;
    mutable std::mutex resizeMutex_;
};
