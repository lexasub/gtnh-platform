#include "LmdbStore.h"
#include <algorithm>
#include <cstring>
#include <lmdb.h>
#include <spdlog/spdlog.h>

// gp-6nmw: every entry point refuses work on an unopened store. Without this,
// a failed mdb_env_open left a non-null env that mdb_txn_begin() dereferences
// (SIGSEGV), and mdb_env_info() inside growMapSize() crashes the same way. One
// check here beats N identical failures deeper in LMDB, and it means a store
// that never opened is inert rather than dangerous.
#define LMDB_REQUIRE_OPEN(fn)                                          \
    if (!envOpen_) [[unlikely]] {                                       \
        spdlog::error("{}: LMDB is not open — refusing the operation", (fn)); \
        return false;                                                  \
    }

#define LMDB_TX_START(txn_var, flags) \
    if (int rc = mdb_txn_begin(env_, nullptr, (flags), &txn_var); rc != 0) { \
        spdlog::error("mdb_txn_begin failed: {}", mdb_strerror(rc)); \
        return false; \
    }

// Commits the current write transaction and reports the outcome to the caller.
// A failed commit means the write never reached disk, so it MUST be propagated:
// falling through here reported success for a lost write, which made the flush
// thread count an unsaved chunk as saved and handed the client a successful
// SaveChunkResp. LMDB has already aborted the transaction on this path, so
// there is nothing left to clean up. `op` names the caller's operation so the
// log says which write is at risk.
#define LMDB_TX_COMMIT(op) \
    if (int rc = mdb_txn_commit(txn); rc != 0) [[unlikely]] { \
        spdlog::error("{}: mdb_txn_commit failed: {} — data NOT persisted", \
                      (op), mdb_strerror(rc)); \
        return false; \
    }

// Upper bound for the initial map. A db file already larger than this is not
// truncated: mdb_env_open re-expands the map to the file's real size, so
// starting small only costs one growMapSize() later.
constexpr size_t DEFAULT_MAP_SIZE = 4ULL * 1024 * 1024 * 1024;

// gp-6nmw: the constructor now reports whether the store is usable. The old
// void open_() returned early on every mdb_env_* failure and left env_ non-null
// but UNOPENED with dbi_ == 0 — indistinguishable from a healthy store. The
// first mdb_txn_begin() on such an env dereferences a null map pointer and
// takes SIGSEGV, so the old behaviour was not "quietly discarding writes", it
// was a crash. close_() resets everything, so the destructor's close_() is a
// no-op when open_() failed.
bool LmdbStore::open_() {
    int rc = mdb_env_create(&env_);
    if (rc != 0) [[unlikely]] {
        spdlog::error("mdb_env_create failed: {}", mdb_strerror(rc));
        env_ = nullptr;
        return false;
    }
    if ((rc = mdb_env_set_maxdbs(env_, 1024)) != 0 ||
        (rc = mdb_env_set_maxreaders(env_, 1024)) != 0) [[unlikely]] {
        spdlog::error("mdb_env_set_maxdbs/maxreaders failed: {}", mdb_strerror(rc));
        close_();
        return false;
    }
    // Honour the caller's cap up front instead of the hardcoded 4 GiB: with the
    // cap ignored, --db-max-size-mb is a lie until growth runs out of room.
    if ((rc = mdb_env_set_mapsize(env_, std::min(maxMapSize_, DEFAULT_MAP_SIZE))) != 0) [[unlikely]] {
        spdlog::error("mdb_env_set_mapsize failed: {}", mdb_strerror(rc));
        close_();
        return false;
    }

    unsigned int env_flags = MDB_NOTLS;
    // TODO(registry-migration): LMDB chunks may contain pre-migration packed
    // item IDs (data/registry/item-id-migration.csv); old saves need a remap
    // pass before they are safe to load.
    if ((rc = mdb_env_open(env_, db_path_.c_str(), env_flags, 0664)) != 0) [[unlikely]] {
        spdlog::error("mdb_env_open failed: {}", mdb_strerror(rc));
        close_();
        return false;
    }

    // Bootstrap the unnamed database. A failure here is fatal for the store:
    // committing anyway left dbi_ indeterminate and no later check would notice.
    MDB_txn* txn = nullptr;
    if ((rc = mdb_txn_begin(env_, nullptr, 0, &txn)) != 0) [[unlikely]] {
        spdlog::error("open_: mdb_txn_begin failed: {}", mdb_strerror(rc));
        close_();
        return false;
    }
    if ((rc = mdb_dbi_open(txn, nullptr, 0, &dbi_)) != 0) [[unlikely]] {
        spdlog::error("mdb_dbi_open failed: {}", mdb_strerror(rc));
        mdb_txn_abort(txn);
        close_();
        return false;
    }
    if ((rc = mdb_txn_commit(txn)) != 0) [[unlikely]] {
        // The DBI handle is only valid once its transaction commits; a failed
        // commit leaves it unusable, so the store must not be marked open.
        spdlog::error("open_: mdb_txn_commit failed: {} — dbi_ is NOT usable",
                      mdb_strerror(rc));
        close_();
        return false;
    }

    envOpen_ = true;
    return true;
}

LmdbStore::LmdbStore(const std::string& db_path, size_t max_map_size)
    : db_path_(db_path), maxMapSize_(max_map_size) {
    envOpen_ = open_();
    if (!envOpen_) [[unlikely]] {
        spdlog::critical("LmdbStore: '{}' is UNUSABLE — every read and write will "
                         "be refused until the database is fixed", db_path_);
    }
}

LmdbStore::~LmdbStore() {
    close_();
}

void LmdbStore::close_() {
    envOpen_ = false;
    if (!env_) return;
    if (dbi_ != 0) {
        mdb_close(env_, dbi_);
        dbi_ = 0;
    }
    mdb_env_close(env_);
    env_ = nullptr;
}

bool LmdbStore::HasChunk(ChunkCoord c) const {
    if (!envOpen_) [[unlikely]] {
        spdlog::error("HasChunk: LMDB is not open — reporting no chunk");
        return false;
    }
    MDB_txn* txn = nullptr;
    LMDB_TX_START(txn, MDB_RDONLY);

    int64_t key = makeKey(c.x, c.y, c.z);
    MDB_val key_val = {sizeof(int64_t), &key};
    MDB_val data_val = {};

    int rc = mdb_get(txn, dbi_, &key_val, &data_val);
    mdb_txn_abort(txn);
    return rc == 0;
}

bool LmdbStore::writeRaw(int64_t key, const uint8_t* data, size_t size) {
    LMDB_REQUIRE_OPEN("writeRaw");
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

        LMDB_TX_COMMIT("writeRaw");
        return true;
    }

    spdlog::error("writeRaw: MDB_MAP_FULL after resize");
    return false;
}

// gp-wjmb: the three outcomes are now distinct. MDB_NOTFOUND is a real answer
// ("this chunk is not stored"); every other failure is ReadError::Failed, which
// callers must not mistake for absence. Returning one std::nullopt for both is
// what let a transient read error destroy a whole chunk.
std::expected<std::vector<uint8_t>, ReadError> LmdbStore::readRawBytes(int64_t key) const {
    if (!envOpen_) [[unlikely]] {
        spdlog::error("readRawBytes: LMDB is not open — cannot read the chunk");
        return std::unexpected(ReadError::Failed);
    }
    MDB_txn* txn = nullptr;
    if (int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn); rc != 0) {
        spdlog::error("mdb_txn_begin failed: {}", mdb_strerror(rc));
        return std::unexpected(ReadError::Failed);
    }

    MDB_val key_val = {sizeof(int64_t), &key};
    MDB_val data_val = {};

    int rc = mdb_get(txn, dbi_, &key_val, &data_val);
    if (rc == MDB_NOTFOUND) {
        mdb_txn_abort(txn);
        return std::unexpected(ReadError::NotFound);
    }
    if (rc != 0) [[unlikely]] {
        spdlog::error("mdb_get failed: {}", mdb_strerror(rc));
        mdb_txn_abort(txn);
        return std::unexpected(ReadError::Failed);
    }

    std::vector<uint8_t> result(
        static_cast<const uint8_t*>(data_val.mv_data),
        static_cast<const uint8_t*>(data_val.mv_data) + data_val.mv_size);
    mdb_txn_abort(txn);
    return result;
}

bool LmdbStore::growMapSize() {
    // mdb_env_info() on a non-open env dereferences a null map pointer and
    // crashes, so the open check has to come before it, not after.
    if (!envOpen_) [[unlikely]] {
        spdlog::error("growMapSize: LMDB is not open — cannot grow the map");
        return false;
    }
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

// gp-jzgr: writeBatch must survive map exhaustion the way writeRaw does, and
// must never throw away chunks that did fit.
//
// The transaction is shared by every entry, so a single MDB_MAP_FULL used to
// abort the whole batch: one entry that did not fit destroyed the chunks that
// did. The loop below grows the map and rewrites the batch, exactly as
// writeRaw already did. If the map is already at the configured cap there is no
// growth left, and the batch is written one chunk per transaction so that
// whatever fits is still persisted. Either way `items` comes back holding
// exactly the entries that are not on disk, and false is returned — so the
// caller retries real work instead of silently dropping it.
bool LmdbStore::writeBatch(
    std::vector<std::pair<int64_t, std::shared_ptr<std::vector<uint8_t>>>>& items) {
    //if (items.empty()) return true;//guaranted is not empty
    // Refuse before touching the env, and leave `items` intact so the caller
    // can retry or re-queue them.
    LMDB_REQUIRE_OPEN("writeBatch");
    if (items.empty()) return true;

    // Upper bound on grow-and-rewrite rounds, so a pathological map cannot spin.
    constexpr int kMaxRounds = 8;

    for (int round = 0; round < kMaxRounds; ++round) {
        MDB_txn* txn = nullptr;
        LMDB_TX_START(txn, 0);

        size_t i = 0;
        for (; i < items.size(); ++i) {
            auto& [key, pal] = items[i];
            MDB_val key_val = {sizeof(int64_t), &key};
            MDB_val data_val = {pal->size(), pal->data()};
            int rc = mdb_put(txn, dbi_, &key_val, &data_val, 0);
            if (rc == MDB_MAP_FULL) [[unlikely]] break;
            if (rc != 0) [[unlikely]] {
                // Not a map error, so growing cannot help. The transaction is
                // still valid here, so persist the entries already accepted
                // and hand back the rest.
                spdlog::error("writeBatch: mdb_put failed at entry {} of {}: {}",
                              i, items.size(), mdb_strerror(rc));
                if (i > 0) {
                    LMDB_TX_COMMIT("writeBatch (partial)");
                    items.erase(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    mdb_txn_abort(txn);
                }
                return false;
            }
        }

        // Everything fit: one commit covers the whole batch.
        if (i == items.size()) {
            LMDB_TX_COMMIT("writeBatch");
            items.clear();
            return true;
        }

        // The map filled mid-batch. LMDB has already poisoned this
        // transaction — after MDB_MAP_FULL it must be ABORTED, not committed
        // (a commit attempt returns MDB_BAD_TXN) — so this round's writes are
        // lost with it and the batch is rewritten after growing. One rewrite
        // buys "no chunk is ever dropped", and the whole batch then commits in
        // a single transaction.
        mdb_txn_abort(txn);
        if (!growMapSize()) {
            // At the cap: write what fits, one chunk per transaction, so the
            // chunks that do fit are not thrown away with the batch.
            if (persistPrefixAndReturnRemainder(items)) return false;
            spdlog::error("writeBatch: cannot grow the map and nothing fit — "
                          "all {} chunks are returned for retry", items.size());
            return false;
        }
        spdlog::warn("writeBatch: MDB_MAP_FULL at entry {} of {} — grew the map, "
                     "rewriting the batch", i, items.size());
    }

    spdlog::error("writeBatch: still MDB_MAP_FULL after {} grow rounds — all {} "
                  "chunks are returned for retry", kMaxRounds, items.size());
    return false;
}

// gp-jzgr last resort, used only when the map is full AND at the configured
// cap, so growth cannot help.
//
// LMDB cannot commit a transaction that has already hit MDB_MAP_FULL, so the
// oversized batch has to be written in several smaller transactions. This walks
// the batch, committing each chunk in its own transaction, and stops at the
// first one that will not fit — so the chunks that DID fit are on disk rather
// than discarded. The entries from that point on are left in `items`.
//
// Returns true if at least one chunk was committed (so the caller should report
// partial progress), false if not a single chunk fit.
bool LmdbStore::persistPrefixAndReturnRemainder(
    std::vector<std::pair<int64_t, std::shared_ptr<std::vector<uint8_t>>>>& items) {
    size_t done = 0;
    for (; done < items.size(); ++done) {
        MDB_txn* txn = nullptr;
        LMDB_TX_START(txn, 0);
        auto& [key, pal] = items[done];
        MDB_val key_val = {sizeof(int64_t), &key};
        MDB_val data_val = {pal->size(), pal->data()};
        int rc = mdb_put(txn, dbi_, &key_val, &data_val, 0);
        if (rc != 0) {
            mdb_txn_abort(txn);
            break;
        }
        LMDB_TX_COMMIT("writeBatch (per-chunk)");
    }
    if (done == 0) return false;
    spdlog::error("writeBatch: map is at its cap — persisted {} of {} chunks, "
                  "{} returned for retry", done, items.size(), items.size() - done);
    items.erase(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(done));
    return true;
}

int64_t LmdbStore::makeKey(int32_t cx, int32_t cy, int32_t cz) {
    uint64_t x = static_cast<uint64_t>(static_cast<int64_t>(cx) + CHUNK_KEY_BIAS);
    uint64_t y = static_cast<uint64_t>(static_cast<int64_t>(cy) + CHUNK_KEY_BIAS);
    uint64_t z = static_cast<uint64_t>(static_cast<int64_t>(cz) + CHUNK_KEY_BIAS);
    return static_cast<int64_t>((x << 42) | (y << 21) | z);
}
