#include "EntityStateStorage.h"
#include "core_generated.h"
#include <stdexcept>
#include <spdlog/spdlog.h>

EntityStateStorage::EntityStateStorage(const std::string& lmdbPath, asio::io_context& io_context, size_t cacheSize)
    : lmdbPath_(lmdbPath), cache_(cacheSize),
      messageRouterClient_(io_context),
      chunkStoreClient_(io_context),
      ioContext_(io_context) {}

EntityStateStorage::~EntityStateStorage() {
    shutdown();
}

bool EntityStateStorage::initialize() {
    spdlog::info("EntityStateStorage initializing at {}...", lmdbPath_);

    int rc = mdb_env_create(&env_);
    if (rc) {
        spdlog::error("Failed to create LMDB environment: {}", mdb_strerror(rc));
        return false;
    }

    rc = mdb_env_set_mapsize(env_, 1024ULL * 1024 * 1024);
    if (rc) {
        spdlog::error("Failed to set LMDB mapsize: {}", mdb_strerror(rc));
        return false;
    }

    rc = mdb_env_open(env_, lmdbPath_.c_str(), MDB_FIXEDMAP | MDB_NOSUBDIR, 0664);
    if (rc) {
        spdlog::error("Failed to open LMDB environment at {}: {}", lmdbPath_, mdb_strerror(rc));
        return false;
    }

    spdlog::info("EntityStateStorage initialized");
    return true;
}

void EntityStateStorage::shutdown() {
    spdlog::info("EntityStateStorage shutting down...");
    cache_.shutdown();
    if (env_) {
        mdb_env_close(env_);
        env_ = nullptr;
    }
}

std::string EntityStateStorage::makeKey(int32_t dimension, int32_t x, int32_t y, int32_t z, uint16_t entityType) {
    return std::to_string(dimension) + "|" +
           std::to_string(x) + "|" +
           std::to_string(y) + "|" +
           std::to_string(z) + "|" +
           std::to_string(entityType);
}

bool EntityStateStorage::LoadEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z,
                                      uint16_t entityType, std::vector<uint8_t>& stateData) {
    std::string key = makeKey(dimension, x, y, z, entityType);

    if (cache_.get(key, stateData)) {
        spdlog::debug("Cache hit for {}", key);
        return true;
    }

    bool result = doLoadEntityState(dimension, x, y, z, entityType, stateData);
    if (result) {
        cache_.set(key, stateData);
    }
    return result;
}

bool EntityStateStorage::SaveEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z,
                                       uint16_t entityType, const std::vector<uint8_t>& stateData) {
    std::string key = makeKey(dimension, x, y, z, entityType);

    cache_.set(key, stateData);
    spdlog::debug("Saved entity state for {}", key);
    return doSaveEntityState(dimension, x, y, z, entityType, stateData);
}

bool EntityStateStorage::DeleteEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z,
                                        uint16_t entityType) {
    std::string key = makeKey(dimension, x, y, z, entityType);

    cache_.remove(key);
    spdlog::debug("Deleted entity state for {}", key);
    return doDeleteEntityState(dimension, x, y, z, entityType);
}

bool EntityStateStorage::doLoadEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z,
                                          uint16_t entityType, std::vector<uint8_t>& stateData) {
    MDB_txn* txn = nullptr;
    MDB_dbi dbi;
    MDB_val keyVal, dataVal;
    int rc;

    rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
    if (rc) {
        spdlog::debug("LMDB read failed: {}", mdb_strerror(rc));
        return false;
    }

    std::string keyStr = makeKey(dimension, x, y, z, entityType);

    keyVal.mv_size = keyStr.size();
    keyVal.mv_data = const_cast<void*>(static_cast<const void*>(keyStr.c_str()));

    rc = mdb_dbi_open(txn, nullptr, 0, &dbi);
    if (rc) {
        mdb_txn_abort(txn);
        return false;
    }

    rc = mdb_get(txn, dbi, &keyVal, &dataVal);
    if (rc == MDB_NOTFOUND) {
        mdb_txn_abort(txn);
        return false;
    }
    if (rc) {
        mdb_txn_abort(txn);
        return false;
    }

    stateData.assign(static_cast<char*>(dataVal.mv_data),
                     static_cast<char*>(dataVal.mv_data) + dataVal.mv_size);

    mdb_txn_abort(txn);
    return true;
}

bool EntityStateStorage::doSaveEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z,
                                         uint16_t entityType, const std::vector<uint8_t>& stateData) {
    MDB_txn* txn = nullptr;
    MDB_dbi dbi;
    MDB_val keyVal, dataVal;
    int rc;

    rc = mdb_txn_begin(env_, nullptr, 0, &txn);
    if (rc) {
        spdlog::debug("LMDB write failed: {}", mdb_strerror(rc));
        return false;
    }

    std::string keyStr = makeKey(dimension, x, y, z, entityType);

    keyVal.mv_size = keyStr.size();
    keyVal.mv_data = const_cast<void*>(static_cast<const void*>(keyStr.c_str()));

    dataVal.mv_size = stateData.size();
    dataVal.mv_data = const_cast<void*>(static_cast<const void*>(stateData.data()));

    rc = mdb_dbi_open(txn, nullptr, 0, &dbi);
    if (rc) {
        mdb_txn_abort(txn);
        return false;
    }

    rc = mdb_put(txn, dbi, &keyVal, &dataVal, 0);
    if (rc) {
        mdb_txn_abort(txn);
        return false;
    }

    rc = mdb_txn_commit(txn);
    if (rc) {
        spdlog::debug("LMDB commit failed: {}", mdb_strerror(rc));
        return false;
    }

    return true;
}

bool EntityStateStorage::doDeleteEntityState(int32_t dimension, int32_t x, int32_t y, int32_t z,
                                           uint16_t entityType) {
    MDB_txn* txn = nullptr;
    MDB_dbi dbi;
    MDB_val keyVal;
    int rc;

    rc = mdb_txn_begin(env_, nullptr, 0, &txn);
    if (rc) {
        spdlog::debug("LMDB delete failed: {}", mdb_strerror(rc));
        return false;
    }

    std::string keyStr = makeKey(dimension, x, y, z, entityType);

    keyVal.mv_size = keyStr.size();
    keyVal.mv_data = const_cast<void*>(static_cast<const void*>(keyStr.c_str()));

    rc = mdb_dbi_open(txn, nullptr, 0, &dbi);
    if (rc) {
        mdb_txn_abort(txn);
        return false;
    }

    rc = mdb_del(txn, dbi, &keyVal, nullptr);
    if (rc) {
        mdb_txn_abort(txn);
        return false;
    }

    rc = mdb_txn_commit(txn);
    if (rc) {
        spdlog::debug("LMDB delete commit failed: {}", mdb_strerror(rc));
        return false;
    }

    return true;
}