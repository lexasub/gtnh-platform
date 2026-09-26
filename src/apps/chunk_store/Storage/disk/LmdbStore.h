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

    // gp-6nmw: false means the LMDB environment could not be opened and this
    // object is inert — every entry point below refuses work until it is
    // rebuilt. Checked once by the daemon at startup so a bad db path is a
    // startup failure instead of a runtime mystery.
    bool isOpen() const noexcept { return envOpen_; }

    bool HasChunk(ChunkCoord c) const override;
    bool writeRaw(int64_t key, const uint8_t* data, size_t size) override;
    bool writeBatch(
        std::vector<std::pair<int64_t, std::shared_ptr<std::vector<uint8_t>>>>& items) override;

    std::expected<std::vector<uint8_t>, ReadError> readRawBytes(
        int64_t key) const override;

    static int64_t makeKey(int32_t cx, int32_t cy, int32_t cz);
private:
    bool growMapSize();
    // gp-jzgr: last resort when the map is full and at the cap. Commits each
    // chunk in its own transaction until one does not fit, then leaves the
    // unwritten tail in `items`. Returns true if anything was committed.
    bool persistPrefixAndReturnRemainder(
        std::vector<std::pair<int64_t, std::shared_ptr<std::vector<uint8_t>>>>& items);
    // gp-6nmw: reports whether the env is usable. Never throws and never
    // leaves a half-open env behind: on any failure it closes what it built
    // and resets env_/dbi_ so the destructor's close_() is a no-op.
    bool open_();
    void close_();

    MDB_env* env_ = nullptr;
    MDB_dbi dbi_ = 0;
    std::string db_path_;
    size_t maxMapSize_;
    // Named envOpen_ to avoid colliding with the open_() method.
    bool envOpen_ = false;
    mutable std::mutex resizeMutex_;
};
