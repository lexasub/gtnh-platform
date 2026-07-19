#include "ChunkStore.h"
#include "cache/MutableChunk.h"
#include "../../world_generator/GenerationQueue.h"
#include "../../world_generator/WorldGenerator.h"
#include <pthread.h>
#include <spdlog/spdlog.h>

ChunkStore::ChunkStore(const std::string& db_path, size_t, size_t max_map_size)
    : lmdb_(db_path, max_map_size),
      cas_(cache_, lmdb_, [this](int64_t key) {
          {
              std::lock_guard lock(encoder_.lmdb_palette_mutex_);
              encoder_.pending_lmdb_.erase(key);
          }
          flusher_.markDirty(key);
      }) {
    io_threads_.emplace_back([this] {
        pthread_setname_np(pthread_self(), "chunk-io");
        io_pool_.run();
    });
    io_threads_.emplace_back([this] {
        pthread_setname_np(pthread_self(), "chunk-io-2");
        io_pool_.run();
    });

    encoder_.start(cache_, lmdb_);
    flusher_.start(cache_, lmdb_, &encoder_.pending_lmdb_, &encoder_.lmdb_palette_mutex_);
    generator_ = new WorldGenerator;
    gen_queue_ = new GenerationQueue(generator_,
                                               [this](ChunkCoord coord, MutableChunk* chunk) {
                                                   enqueueEncode(coord, chunk);
                                               });
    encoder_.SetGenerationQueue(gen_queue_);
}

ChunkStore::~ChunkStore() {
    close();
    delete gen_queue_;
    delete generator_;
}

void ChunkStore::close() {
    work_guard_.reset();
    io_pool_.stop();
    for (auto& t : io_threads_) {
        if (t.joinable()) t.join();
    }
    io_threads_.clear();

    encoder_.stop();
    flusher_.stop();
    cache_.clear();
}

bool ChunkStore::HasChunk(ChunkCoord c) const {
    auto cached = getCached(c.x, c.y, c.z);
    if (cached) return true;
    return lmdb_.HasChunk(c);
}

const MutableChunk* ChunkStore::GetChunk(ChunkCoord c) const {
    auto* cached = getCached(c.x, c.y, c.z);
    if (cached) return cached;

    auto wire = lmdb_.readRawBytes(makeKey(c.x, c.y, c.z));
    if (!wire) {
        // Not in LMDB either — return empty chunk (old behavior for new chunks).
        MutableChunk* chunk = cache_.takeFromPool();
        if (!chunk) chunk = new MutableChunk();
        cache_.put(makeKey(c.x, c.y, c.z), chunk);
        return chunk;
    }

    MutableChunk* chunk = cache_.takeFromPool();
    if (!chunk) chunk = new MutableChunk();
    if (!chunk->fromWire(wire->data(), wire->size())) {
        delete chunk;
        return nullptr;
    }
    cache_.put(makeKey(c.x, c.y, c.z), chunk);
    return chunk;
}

uint16_t ChunkStore::GetBlockAt(BlockPos pos) const {
    auto chunk = GetChunk({pos.x >> 5, pos.y >> 5, pos.z >> 5});
    if (!chunk) return 0;
    return chunk->getBlock(pos.x & 31, pos.y & 31, pos.z & 31);
}

uint8_t ChunkStore::GetMeta(int32_t x, int32_t y, int32_t z) const {
    auto chunk = GetChunk({x >> 5, y >> 5, z >> 5});
    if (!chunk) return 0;
    return chunk->getMeta(x & 31, y & 31, z & 31);
}

uint32_t ChunkStore::GetMultiblock(int32_t x, int32_t y, int32_t z) const {
    auto chunk = GetChunk({x >> 5, y >> 5, z >> 5});
    if (!chunk) return 0;
    return chunk->getMultiblock(x & 31, y & 31, z & 31);
}

ChunkStore::CASResult ChunkStore::casBlock(int32_t x, int32_t y, int32_t z,
                                            uint16_t expected_id,
                                            uint16_t new_id, uint8_t new_meta) {
    return cas_.casBlock(x, y, z, expected_id, new_id, new_meta);
}

void ChunkStore::setBlock(int32_t x, int32_t y, int32_t z, uint16_t id, uint8_t meta) {
    int32_t cx = x >> 5;
    int32_t cy = y >> 5;
    int32_t cz = z >> 5;
    int32_t lx = x & 31;
    int32_t ly = y & 31;
    int32_t lz = z & 31;
    int64_t key = makeKey(cx, cy, cz);

    MutableChunk* chunk = const_cast<MutableChunk*>(getCached(cx, cy, cz));
    if (!chunk) {
        auto wire = lmdb_.readRawBytes(key);
        MutableChunk local;
        if (wire) local.fromWire(wire->data(), wire->size());
        local.setBlock(lx, ly, lz, id);
        local.setMeta(lx, ly, lz, meta);
        chunk = cache_.takeFromPool();
        if (!chunk) {
            chunk = new MutableChunk(std::move(local));
        } else {
            *chunk = std::move(local);
        }
        cache_.put(key, chunk);
        {
            std::lock_guard lock(encoder_.lmdb_palette_mutex_);
            encoder_.pending_lmdb_.erase(key);
        }
        markDirty(cx, cy, cz);
        return;
    }

    chunk->setBlock(lx, ly, lz, id);
    chunk->setMeta(lx, ly, lz, meta);
    {
        std::lock_guard lock(encoder_.lmdb_palette_mutex_);
        encoder_.pending_lmdb_.erase(key);
    }
    markDirty(cx, cy, cz);
}

void ChunkStore::SetBlock(ChunkCoord coord, BlockPos pos, uint16_t blockId,
                          uint8_t meta, uint32_t mbId) {
    (void)pos; (void)mbId;
    setBlock(coord.x, coord.y, coord.z, blockId, meta);
}

bool ChunkStore::SaveChunk(const MutableChunk& chunk, ChunkCoord coord) {
    std::vector<uint8_t> encoded;
    chunk.encodeToWire(encoded);
    return lmdb_.writeRaw(makeKey(coord.x, coord.y, coord.z),
                          encoded.data(), encoded.size());
}

const MutableChunk* ChunkStore::getCached(int32_t cx, int32_t cy, int32_t cz) const {
    return cache_.get(makeKey(cx, cy, cz));
}

const MutableChunk* ChunkStore::getCachedPinned(int32_t cx, int32_t cy, int32_t cz) {
    return cache_.getPinned(makeKey(cx, cy, cz));
}

void ChunkStore::releaseCachedPinned(const MutableChunk* chunk) {
    cache_.releasePinned(chunk);
}

void ChunkStore::putCached(int64_t key, MutableChunk* chunk) const {
    cache_.put(key, chunk);
}

void ChunkStore::AsyncSetBlock(ChunkCoord coord, BlockPos pos,
                               uint16_t blockId, uint8_t meta, uint32_t mbId,
                               std::function<void(bool)> callback) {
    asio::post(io_pool_, [this, coord, pos, blockId, meta, mbId,
                          callback = std::move(callback)]() mutable {
        bool result = false;
        try {
            SetBlock(coord, pos, blockId, meta, mbId);
            result = true;
        } catch (...) {
            result = false;
        }
        if (callback) callback(result);
    });
}

void ChunkStore::AsyncSaveChunk(std::shared_ptr<MutableChunk> chunk, ChunkCoord coord,
                                std::function<void(bool)> callback) {
    asio::post(io_pool_, [this, chunk = std::move(chunk), coord,
                          callback = std::move(callback)]() mutable {
        bool result = false;
        try {
            result = SaveChunk(*chunk, coord);
        } catch (...) {
            result = false;
        }
        if (callback) callback(result);
    });
}

void ChunkStore::markDirty(int32_t cx, int32_t cy, int32_t cz) {
    flusher_.markDirty(makeKey(cx, cy, cz));
}

bool ChunkStore::flushDirtyChunks() {
    return flusher_.flushDirtyChunks();
}

void ChunkStore::enqueueEncode(ChunkCoord coord, MutableChunk* chunk) {
    encoder_.enqueueEncode(coord, chunk);
}

void ChunkStore::AsyncGetChunk(ChunkCoord coord, ChunkCallback callback) {
    int64_t key = makeKey(coord.x, coord.y, coord.z);

    if (auto* cached = getCachedPinned(coord.x, coord.y, coord.z)) {
        if (auto palette = encoder_.takePendingPalette(key)) {
            releaseCachedPinned(cached);
            callback(std::move(palette));
            return;
        }
        std::vector<uint8_t> encoded;
        encoded.reserve(SEC_CNT * (SEC_VOL + 256 * 2 + 16));
        cached->encodeToWire(encoded);
        encoded.shrink_to_fit();
        releaseCachedPinned(cached);
        auto palette = std::make_shared<std::vector<uint8_t>>(std::move(encoded));
        encoder_.markAsPendingLmdb(palette, key);
        callback(std::move(palette));
        return;
    }

    asio::post(io_pool_, [this, coord, key, callback = std::move(callback)]() mutable {
        std::shared_ptr<std::vector<uint8_t>> palette;

        if (auto wire = lmdb_.readRawBytes(key)) {
            MutableChunk* chunk_ptr = cache_.takeFromPool();
            if (!chunk_ptr) chunk_ptr = new MutableChunk();
            if (!chunk_ptr->fromWire(wire->data(), wire->size())) {
                delete chunk_ptr;
                callback(nullptr);
                return;
            }

            palette = std::make_shared<std::vector<uint8_t>>(std::move(*wire));
            cache_.put(key, chunk_ptr);
            callback(std::move(palette));
            return;
        }
        {
            std::lock_guard lock(encoder_.encode_mutex_);
            encoder_.pending_gen_cbs_[key].push_back(std::move(callback));
        }
        gen_queue_->requestChunk(coord);
    });
}
