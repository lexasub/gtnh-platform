#pragma once

#include "IBlockStore.h"
#include <cstdint>
#include <lmdb.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// LMDB-backed block storage. Thread-safe (MDB_NOTLS, per-thread transactions).
class LmdbStore : public IBlockStore {
public:
    explicit LmdbStore(const std::string& db_path, size_t max_map_size);
    ~LmdbStore() override;

    // IBlockStore
    bool HasChunk(ChunkCoord c) const override;
    bool readRaw(int64_t key, Chunk& out,
                 std::shared_ptr<std::vector<uint8_t>>* palette_out = nullptr) const override;
    bool writeRaw(int64_t key, const uint8_t* data, size_t size) override;
    bool writeBatch(
        std::vector<std::pair<int64_t, std::shared_ptr<std::vector<uint8_t>>>>& items) override;

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
