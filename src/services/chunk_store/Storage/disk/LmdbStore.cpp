#include "LmdbStore.h"
#include "../SectionCodec.h"
#include <cstring>
#include <lmdb.h>
#include <spdlog/spdlog.h>

#define LMDB_TX_START(txn_var, flags) \
    if (int rc = mdb_txn_begin(env_, nullptr, (flags), &txn_var); rc != 0) { \
        spdlog::error("mdb_txn_begin failed: {}", mdb_strerror(rc)); \
        return false; \
    }

#define LMDB_TX_COMMIT() \
    if (int rc = mdb_txn_commit(txn); rc != 0) [[unlikely]] { \
        spdlog::error("mdb_txn_commit failed: {}", mdb_strerror(rc)); \
    }

constexpr uint64_t DEFAULT_MAP_SIZE = 4ULL * 1024 * 1024 * 1024;

LmdbStore::LmdbStore(const std::string& db_path, size_t max_map_size)
    : db_path_(db_path), maxMapSize_(max_map_size) {
    open_();
}

LmdbStore::~LmdbStore() {
    close_();
}

void LmdbStore::open_() {
    int rc;
    if (rc = mdb_env_create(&env_); rc != 0) {
        spdlog::error("mdb_env_create failed: {}", mdb_strerror(rc));
        return;
    }
    if (rc = mdb_env_set_maxdbs(env_, 1024); rc != 0) {
        spdlog::error("mdb_env_set_maxdbs failed: {}", mdb_strerror(rc));
        return;
    }
    if (rc = mdb_env_set_mapsize(env_, DEFAULT_MAP_SIZE); rc != 0) {
        spdlog::error("mdb_env_set_mapsize failed: {}", mdb_strerror(rc));
        return;
    }
    if (rc = mdb_env_set_maxreaders(env_, 1024); rc != 0) {
        spdlog::error("mdb_env_set_maxreaders failed: {}", mdb_strerror(rc));
        return;
    }

    unsigned int env_flags = MDB_NOTLS;
    if (rc = mdb_env_open(env_, db_path_.c_str(), env_flags, 0664); rc != 0) {
        spdlog::error("mdb_env_open failed: {}", mdb_strerror(rc));
        return;
    }

    MDB_txn* txn = nullptr;
    if (rc = mdb_txn_begin(env_, nullptr, 0, &txn); rc == 0) {
        if (int rc2 = mdb_dbi_open(txn, nullptr, 0, &dbi_); rc2 != 0) {
            spdlog::error("mdb_dbi_open failed: {}", mdb_strerror(rc2));
        }
        mdb_txn_commit(txn);
    } else {
        spdlog::error("Failed to open DBI during init: {}", mdb_strerror(rc));
    }
}

void LmdbStore::close_() {
    if (!env_) return;
    if (dbi_ != 0) {
        mdb_close(env_, dbi_);
        dbi_ = 0;
    }
    mdb_env_close(env_);
    env_ = nullptr;
}

bool LmdbStore::HasChunk(ChunkCoord c) const {
    MDB_txn* txn = nullptr;
    LMDB_TX_START(txn, MDB_RDONLY);

    int64_t key = makeKey(c.x, c.y, c.z);
    MDB_val key_val = {sizeof(int64_t), &key};
    MDB_val data_val = {};

    int rc = mdb_get(txn, dbi_, &key_val, &data_val);
    mdb_txn_abort(txn);
    return rc == 0;
}

bool LmdbStore::readRaw(int64_t key, Chunk& out,
                         std::shared_ptr<std::vector<uint8_t>>* palette_out) const {
    MDB_txn* txn = nullptr;
    LMDB_TX_START(txn, MDB_RDONLY);

    MDB_val key_val = {sizeof(int64_t), &key};
    MDB_val data_val = {};

    int rc = mdb_get(txn, dbi_, &key_val, &data_val);

    if (rc == MDB_NOTFOUND) {
        out = Chunk{};
        mdb_txn_commit(txn);
        return false;
    }

    if (rc != 0) {
        spdlog::error("mdb_get failed: {}", mdb_strerror(rc));
        mdb_txn_abort(txn);
        return false;
    }

    if (data_val.mv_size >= 6 &&
        decodeChunk(static_cast<const uint8_t*>(data_val.mv_data),
                    data_val.mv_size, out)) {
        if (palette_out) {
            auto palette = std::make_shared<std::vector<uint8_t>>(
                static_cast<const uint8_t*>(data_val.mv_data),
                static_cast<const uint8_t*>(data_val.mv_data) + data_val.mv_size);
            *palette_out = std::move(palette);
        }
        LMDB_TX_COMMIT();
        return true;
    }

    // old format
    if (data_val.mv_size == sizeof(Chunk)) {
        std::memcpy(&out, data_val.mv_data, sizeof(Chunk));
        LMDB_TX_COMMIT();
        return true;
    }

    spdlog::error("Data size mismatch for key {}. Expected section or {}, got {}",
                  key, sizeof(Chunk), data_val.mv_size);
    out = Chunk{};
    mdb_txn_abort(txn);
    return false;
}

bool LmdbStore::writeRaw(int64_t key, const uint8_t* data, size_t size) {
    MDB_txn* txn = nullptr;
    MDB_val data_val = {size, const_cast<uint8_t*>(data)};
    for (int attempt = 0; attempt < 2; ++attempt) {
        LMDB_TX_START(txn, 0);
        MDB_val key_val = {sizeof(int64_t), &key};

        int rc = mdb_put(txn, dbi_, &key_val, &data_val, 0);
        if (rc == MDB_MAP_FULL) {
            mdb_txn_abort(txn);
            txn = nullptr;
            if (!growMapSize())
                return false;
            continue;
        }

        if (rc != 0) [[unlikely]] {
            spdlog::error("mdb_put failed: {}", mdb_strerror(rc));
            mdb_txn_abort(txn);
            return false;
        }

        LMDB_TX_COMMIT();
        return true;
    }

    spdlog::error("writeRaw: MDB_MAP_FULL after resize");
    return false;
}

bool LmdbStore::growMapSize() {
    std::lock_guard lock(resizeMutex_);

    MDB_envinfo info;
    if (int rc = mdb_env_info(env_, &info); rc != 0) {
        spdlog::error("growMapSize: mdb_env_info failed: {}", mdb_strerror(rc));
        return false;
    }

    size_t old_size = info.me_mapsize;
    size_t new_size = std::min(old_size * 2, maxMapSize_);
    if (new_size <= old_size) {
        spdlog::error("growMapSize: already at max mapsize ({} bytes)", old_size);
        return false;
    }

    spdlog::warn("growMapSize: {} -> {} bytes (max: {})", old_size, new_size, maxMapSize_);
    if (int rc = mdb_env_set_mapsize(env_, new_size); rc != 0) {
        spdlog::error("growMapSize: mdb_env_set_mapsize failed: {}", mdb_strerror(rc));
        return false;
    }
    return true;
}

bool LmdbStore::writeBatch(
    std::vector<std::pair<int64_t, std::shared_ptr<std::vector<uint8_t>>>>& items) {
    //if (items.empty()) return true;//guaranted is not empty
    MDB_txn* txn = nullptr;
    LMDB_TX_START(txn, 0);
    for (auto& [key, pal] : items) {
        MDB_val key_val = {sizeof(int64_t), &key};
        MDB_val data_val = {pal->size(), pal->data()};
        if (int rc = mdb_put(txn, dbi_, &key_val, &data_val, 0); rc != 0) {
            spdlog::error("writeBatch: mdb_put failed: {}", mdb_strerror(rc));
            mdb_txn_abort(txn);
            return false;
        }
    }
    LMDB_TX_COMMIT();
    items.clear();
    return true;
}

int64_t LmdbStore::makeKey(int32_t cx, int32_t cy, int32_t cz) {
    uint64_t x = static_cast<uint64_t>(static_cast<int64_t>(cx) + CHUNK_KEY_BIAS);
    uint64_t y = static_cast<uint64_t>(static_cast<int64_t>(cy) + CHUNK_KEY_BIAS);
    uint64_t z = static_cast<uint64_t>(static_cast<int64_t>(cz) + CHUNK_KEY_BIAS);
    return static_cast<int64_t>((x << 42) | (y << 21) | z);
}
