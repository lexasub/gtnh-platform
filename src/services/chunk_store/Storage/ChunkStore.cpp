#include "ChunkStore.h"
#include "SectionCodec.h"
#include "../../world_generator/GenerationQueue.h"
#include <cstring>
#include <lmdb.h>
#include <memory>
#include <pthread.h>
#include <spdlog/spdlog.h>

// Хелпер для проверки ошибок LMDB
#define CHECK_LMDB(call, msg) { \
    rc = call; \
    if (rc != 0) { \
        spdlog::error("{}: {}", (msg), mdb_strerror(rc)); \
        return; \
    } \
}

// Макрос для начала транзакции.
// Если ошибка — логирует, делает abort транзакции (если она создалась), прерывает выполнение функции.
#define LMDB_TX_START(txn_var, flags) \
    MDB_txn* txn_var = nullptr; \
    if (int rc = mdb_txn_begin(env_, nullptr, (flags), &txn_var); rc != 0) { \
        spdlog::error("mdb_txn_begin failed: {}", mdb_strerror(rc)); \
        return false; \
    }

// Макрос для проверки вызова функции LMDB.
// Если ошибка — логирует, откатывает текущую транзакцию, прерывает выполнение функции.
#define LMDB_CHECK(call, msg) \
    if (int rc = (call); rc != 0) { \
        spdlog::error("{}: {}", (msg), mdb_strerror(rc)); \
        if (txn) mdb_txn_abort(txn); \
        return; \
    }

// Макрос для завершения транзакции.
// Логирует ошибку, если коммит не удался.
#define LMDB_TX_COMMIT() \
    if (int rc = mdb_txn_commit(txn); rc != 0) { \
        spdlog::error("mdb_txn_commit failed: {}", mdb_strerror(rc)); \
    }

constexpr uint64_t DEFAULT_MAP_SIZE = 4ULL * 1024 * 1024 * 1024; // 4 GB

namespace {
    void evictChunk(uintptr_t val) {
        delete reinterpret_cast<Chunk*>(val);
    }
}

ChunkStore::ChunkStore(const std::string& db_path, size_t, size_t max_map_size)
    : env_(nullptr), dbi_(0), db_path_(db_path), maxMapSize_(max_map_size) {
    cache_.set_on_evict(evictChunk);
    open();

    // Запуск I/O thread pool (2 threads, named for debugging)
    io_threads_.emplace_back([this] {
        pthread_setname_np(pthread_self(), "chunk-io");
        io_pool_.run();
    });
    io_threads_.emplace_back([this] {
        pthread_setname_np(pthread_self(), "chunk-io-2");
        io_pool_.run();
    });

    // Запуск flush thread (batch save of dirty chunks)
    flush_thread_ = std::thread([this] { flushThreadFunc(); });
}

void ChunkStore::open() {
    int rc;
    CHECK_LMDB(mdb_env_create(&env_), "mdb_env_create failed");

    CHECK_LMDB(mdb_env_set_maxdbs(env_, 1024), "mdb_env_set_maxdbs failed");
    CHECK_LMDB(mdb_env_set_mapsize(env_, DEFAULT_MAP_SIZE), "mdb_env_set_mapsize failed");
    CHECK_LMDB(mdb_env_set_maxreaders(env_, 1024), "mdb_env_set_maxreaders failed");

    // MDB_NOTLS: one environment shared across threads, transactions are per-thread
    unsigned int env_flags = MDB_NOTLS;
    CHECK_LMDB(mdb_env_open(env_, db_path_.c_str(), env_flags, 0664), "mdb_env_open failed");

    // Open data DBI once and store it for reuse across transactions
    MDB_txn* txn = nullptr;
    if (int rc2 = mdb_txn_begin(env_, nullptr, 0, &txn); rc2 == 0) {
        CHECK_LMDB(mdb_dbi_open(txn, nullptr, 0, &dbi_), "mdb_dbi_open failed");
        mdb_txn_commit(txn);
    } else {
        spdlog::error("Failed to open DBI during init: {}", mdb_strerror(rc2));
    }
}

void ChunkStore::close() {
    // Stop I/O pool first — ensures all in-flight LMDB operations complete
    work_guard_.reset();
    io_pool_.stop();
    for (auto& t : io_threads_) {
        if (t.joinable()) t.join();
    }
    io_threads_.clear();

    // Stop flush thread (wake from CV wait) and drain remaining dirty chunks
    flush_running_ = false;
    flush_wake_cv_.notify_one();
    if (flush_thread_.joinable()) flush_thread_.join();
    flushDirtyChunks();

    if (env_ == nullptr) return;

    // Clear and close cache first
    cache_.clear();

    if (dbi_ != 0) {
        mdb_close(env_, dbi_);
        dbi_ = 0;
    }

    mdb_env_close(env_);
    env_ = nullptr;
}

ChunkStore::~ChunkStore() {
    close();
}

bool ChunkStore::HasChunk(ChunkCoord c) const {
    auto cached = getCached(c.x, c.y, c.z);
    if (cached) return true;

    MDB_txn* txn = nullptr;
    if (int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn); rc != 0) {
        spdlog::error("mdb_txn_begin failed: {}", mdb_strerror(rc));
        return false;
    }

    int64_t key = makeKey(c.x, c.y, c.z);
    MDB_val key_val = {sizeof(int64_t), &key};
    MDB_val data_val = {};

    int rc = mdb_get(txn, dbi_, &key_val, &data_val);
    mdb_txn_abort(txn);

    return (rc == 0);
}

const Chunk * ChunkStore::GetChunk(ChunkCoord c) const {
    if (auto* cached = getCached(c.x, c.y, c.z))
        return cached;

    auto chunk = new Chunk;
    readTransaction(makeKey(c.x, c.y, c.z), *chunk);

    putCached(c.x, c.y, c.z, chunk);
    return chunk;
}

uint16_t ChunkStore::GetBlockAt(BlockPos pos) const {
    auto chunk = GetChunk({pos.x >> 5, pos.y >> 5, pos.z >> 5});
    return chunk->GetBlock(pos.x & 31, pos.y & 31, pos.z & 31);
}

// Pack chunk coordinates into int64_t using biased encoding.
// CHUNK_KEY_BIAS (from Types.h) shifts negative coords into positive range,
// matching the client's MakeChunkKey. Range: ±2^20 chunks per dimension.
int64_t ChunkStore::makeKey(int32_t cx, int32_t cy, int32_t cz) {
    uint64_t x = static_cast<uint64_t>(static_cast<int64_t>(cx) + CHUNK_KEY_BIAS);
    uint64_t y = static_cast<uint64_t>(static_cast<int64_t>(cy) + CHUNK_KEY_BIAS);
    uint64_t z = static_cast<uint64_t>(static_cast<int64_t>(cz) + CHUNK_KEY_BIAS);
    return static_cast<int64_t>((x << 42) | (y << 21) | z);
}

void ChunkStore::SetBlock(ChunkCoord coord, BlockPos pos, uint16_t blockId, uint8_t meta, uint32_t mbId) {
    (void)pos; (void)mbId;
    setBlock(coord.x, coord.y, coord.z, blockId, meta);//TODO use mbId
}

bool ChunkStore::SaveChunk(const Chunk &chunk, ChunkCoord coord) {
    writeTransaction(makeKey(coord.x, coord.y, coord.z), chunk);
    // Don't erase from cache — the chunk is now up-to-date in both LMDB and cache.
    // Erasing forces a cache miss + LMDB read on the next get for no benefit.
    return true;
}

// Cache helpers
const Chunk* ChunkStore::getCached(int32_t cx, int32_t cy, int32_t cz) const {
    auto opt = cache_.get(makeKey(cx, cy, cz));
    return opt ? reinterpret_cast<const Chunk*>(*opt) : nullptr;
}

void ChunkStore::putCached(int32_t cx, int32_t cy, int32_t cz, Chunk* chunk) const {
    cache_.put(makeKey(cx, cy, cz), reinterpret_cast<uintptr_t>(chunk));
}

// Read from LMDB with proper transaction handling
bool ChunkStore::readTransaction(int64_t key, Chunk &chunk) const {
    LMDB_TX_START(txn, MDB_RDONLY);

    MDB_val key_val = {sizeof(int64_t), &key};
    MDB_val data_val = {};

    int rc = mdb_get(txn, dbi_, &key_val, &data_val);

    if (rc == MDB_NOTFOUND) {
        chunk = Chunk{};
        mdb_txn_commit(txn);
        return false;
    }

    if (rc != 0) {
        spdlog::error("mdb_get failed: {}", mdb_strerror(rc));
        mdb_txn_abort(txn);
        return false;
    }

    // Try new palette-encoded format first, then fall back to raw struct for
    // backward compatibility with pre-refactor data.mdb files.
    if (data_val.mv_size >= 6 && decodeChunk(
            static_cast<const uint8_t*>(data_val.mv_data), data_val.mv_size, chunk)) {
        LMDB_TX_COMMIT();
        return true;
    }

    if (data_val.mv_size == sizeof(Chunk)) {
        std::memcpy(&chunk, data_val.mv_data, sizeof(Chunk));
        LMDB_TX_COMMIT();
        return true;
    }

    spdlog::error("Data size mismatch for key {}. Expected section or {}, got {}",
                  key, sizeof(Chunk), data_val.mv_size);
    chunk = Chunk{};
    mdb_txn_abort(txn);
    return false;
}

// Grow LMDB mapsize when MDB_MAP_FULL is hit.
// Must be called with NO active transactions on any thread.
bool ChunkStore::growMapSize() {
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

    spdlog::warn("growMapSize: {} → {} bytes (max: {})", old_size, new_size, maxMapSize_);
    if (int rc = mdb_env_set_mapsize(env_, new_size); rc != 0) {
        spdlog::error("growMapSize: mdb_env_set_mapsize failed: {}", mdb_strerror(rc));
        return false;
    }
    return true;
}

// Write to LMDB with proper transaction handling + auto-resize on MDB_MAP_FULL.
bool ChunkStore::writeTransaction(int64_t key, const Chunk &chunk) {
    // Encode to palette format — local buffer, NOT shared (thread-safe).
    std::vector<uint8_t> encoded;
    encodeChunk(chunk, encoded);
    MDB_val data_val = {encoded.size(), encoded.data()};

    // Retry loop: on MDB_MAP_FULL we grow the mapsize and retry once.
    for (int attempt = 0; attempt < 2; ++attempt) {
        LMDB_TX_START(txn, 0);
        MDB_val key_val = {sizeof(int64_t), &key};

        int rc = mdb_put(txn, dbi_, &key_val, &data_val, 0);
        if (rc == MDB_MAP_FULL) {
            if (txn) mdb_txn_abort(txn);
            if (!growMapSize())
                return false;
            continue; // retry
        }

        if (rc != 0) {
            spdlog::error("mdb_put failed: {}", mdb_strerror(rc));
            if (txn) mdb_txn_abort(txn);
            return false;
        }

        LMDB_TX_COMMIT();
        return true;
    }

    spdlog::error("writeTransaction: MDB_MAP_FULL after resize");
    return false;
}

uint8_t ChunkStore::GetMeta(int32_t x, int32_t y, int32_t z) const {
    auto chunk = GetChunk({x >> 5, y >> 5, z >> 5});
    return chunk->meta[(y & 31) << 10 | (z & 31) << 5 | (x & 31)];
}

uint32_t ChunkStore::GetMultiblock(int32_t x, int32_t y, int32_t z) const {
    auto chunk = GetChunk({x >> 5, y >> 5, z >> 5});
    return chunk->multiblock[(y & 31) << 10 | (z & 31) << 5 | (x & 31)];
}

void ChunkStore::setBlock(int32_t x, int32_t y, int32_t z, uint16_t id, uint8_t meta) {
    int32_t cx = x >> 5;
    int32_t cy = y >> 5;
    int32_t cz = z >> 5;
    int32_t lx = x & 31;
    int32_t ly = y & 31;
    int32_t lz = z & 31;
    uint32_t idx = (static_cast<uint32_t>(ly) << 10) |
                   (static_cast<uint32_t>(lz) <<  5) |
                   (static_cast<uint32_t>(lx));
    int64_t key = makeKey(cx, cy, cz);

    // Get or create chunk, modify in-place, no cache replacement.
    // In-place eliminates the UAF race: flush thread holds a Chunk*,
    // putCached would evict it → callback deletes → flush thread UAF.
    Chunk* chunk = const_cast<Chunk*>(getCached(cx, cy, cz));
    if (!chunk) {
        Chunk local;
        readTransaction(key, local);
        local.blocks[idx] = id;
        local.meta[idx]   = meta;
        chunk = new Chunk(std::move(local));
        putCached(cx, cy, cz, chunk);
        markDirty(cx, cy, cz);
        spdlog::debug("Block set at [{},{},{}] -> id={} meta={}", x, y, z, id, meta);
        return;
    }

    chunk->blocks[idx] = id;
    chunk->meta[idx]   = meta;
    markDirty(cx, cy, cz);

    spdlog::debug("Block set at [{},{},{}] -> id={} meta={}", x, y, z, id, meta);
}

void ChunkStore::AsyncSetBlock(ChunkCoord coord, BlockPos pos, uint16_t blockId, uint8_t meta, uint32_t mbId,
                               std::function<void(bool)> callback) {
    asio::post(io_pool_, [this, coord, pos, blockId, meta, mbId, callback = std::move(callback)]() mutable {
        bool result = false;
        try {
            SetBlock(coord, pos, blockId, meta, mbId);
            result = true;
        } catch (...) {
            result = false;
        }
        if (callback) {
            callback(result);
        }
    });
}

void ChunkStore::AsyncSaveChunk(std::shared_ptr<const Chunk> chunk, ChunkCoord coord,
                                std::function<void(bool)> callback) {
    asio::post(io_pool_, [this, chunk = std::move(chunk), coord, callback = std::move(callback)]() mutable {
        bool result = false;
        try {
            result = SaveChunk(*chunk, coord);
        } catch (...) {
            result = false;
        }
        if (callback) {
            callback(result);
        }
    });
}

void ChunkStore::markDirty(int32_t cx, int32_t cy, int32_t cz) {
    int64_t key = makeKey(cx, cy, cz);
    std::lock_guard lock(dirty_mutex_);
    dirty_chunks_.insert(key);
}

void ChunkStore::flushDirtyChunks() {
    std::unordered_set<int64_t> local;
    {
        std::lock_guard lock(dirty_mutex_);
        local.swap(dirty_chunks_);
    }
    if (local.empty()) return;

    size_t saved = 0;
    for (const auto& key : local) {
        auto opt = cache_.get(key);
        if (!opt) continue;
        auto* chunk = reinterpret_cast<const Chunk*>(*opt);
        if (writeTransaction(key, *chunk))
            ++saved;
    }
    if (saved > 0)
        spdlog::debug("Flushed {} dirty chunks to LMDB", saved);
}

void ChunkStore::flushThreadFunc() {
    pthread_setname_np(pthread_self(), "chunk-flush");
    while (flush_running_) {
        {
            std::unique_lock lock(flush_wake_mutex_);
            flush_wake_cv_.wait_for(lock, FLUSH_INTERVAL,
                [this]{ return !flush_running_.load(); });
        }
        if (!flush_running_) break;
        flushDirtyChunks();
    }
}

void ChunkStore::AsyncGetChunk(ChunkCoord coord,

    std::move_only_function<void(const Chunk*)> callback) {
    // 1) Check cache first — if hit, callback immediately on caller thread
    if (auto* cached = getCached(coord.x, coord.y, coord.z)) {
        callback(cached);
        return;
    }

    // 2) Post to I/O pool for LMDB read
    asio::post(io_pool_, [this, coord, callback = std::move(callback)]() mutable {
        int64_t key = makeKey(coord.x, coord.y, coord.z);

        // 2a) Try reading from LMDB
        Chunk chunk;
        bool found = readTransaction(key, chunk);

        // 2b) Found in LMDB — use as-is (even if all-zeros / air chunk). Only
        //     request generation if the chunk has never been generated before.
        if (found) {
            auto* chunk_ptr = new Chunk(std::move(chunk));
            putCached(coord.x, coord.y, coord.z, chunk_ptr);
            callback(chunk_ptr);
            return;
        }

        // 2c) Not found — request generation
        if (!gen_queue_) {
            // No generator — return empty chunk
            auto* empty = new Chunk();
            putCached(coord.x, coord.y, coord.z, empty);
            callback(empty);
            return;
        }
      gen_queue_->requestChunk(coord,
                                  [this, coord, callback = std::move(callback)](std::shared_ptr<Chunk> gen_chunk) mutable {
                                      // Save generated chunk to LMDB (on gen thread)
                                      writeTransaction(makeKey(coord.x, coord.y, coord.z), *gen_chunk);
                                      // Cache a copy (gen_queue owns the original shared_ptr)
                                      auto* cached_chunk = new Chunk();
                                      std::memcpy(cached_chunk, gen_chunk.get(), sizeof(Chunk));
                                      putCached(coord.x, coord.y, coord.z, cached_chunk);
                                      callback(cached_chunk);
                                  });
    });
}