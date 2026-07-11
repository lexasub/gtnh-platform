#include "ChunkStore.h"
#include "SectionCodec.h"
#include "../../world_generator/GenerationQueue.h"
#include "../../world_generator/WorldGenerator.h"
#include <pthread.h>
#include <spdlog/spdlog.h>

ChunkStore::ChunkStore(const std::string& db_path, size_t, size_t max_map_size)
    : lmdb_(db_path, max_map_size),
      cas_(cache_, lmdb_, [this](int64_t key) {
          // Invalidate stale palette + mark dirty for flush pipeline
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

    // Wire up encode pipeline:
    //   on_encoded callback stores palette for AsyncGetChunk fast path.
    encoder_.start(cache_, lmdb_, [this](int64_t key, auto palette) {
        std::lock_guard lock(encoder_.lmdb_palette_mutex_);
        encoder_.pending_lmdb_.emplace(key, std::move(palette));
    });

    // Wire up flush pipeline — reads palettes from encoder's pending_lmdb_.
    flusher_.start(cache_, lmdb_, &encoder_.pending_lmdb_, &encoder_.lmdb_palette_mutex_);
    generator_ = new WorldGenerator;
    gen_queue_ = new GenerationQueue(generator_,
                                               [this](ChunkCoord coord, Chunk* chunk) {
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

const Chunk* ChunkStore::GetChunk(ChunkCoord c) const {
    auto* cached = getCached(c.x, c.y, c.z);
    if (cached) return cached;

    auto chunk = new Chunk();
    if (lmdb_.readRaw(makeKey(c.x, c.y, c.z), *chunk)) {
        cache_.put(makeKey(c.x, c.y, c.z), chunk);
        return chunk;
    }
    delete chunk;
    return nullptr;
}

uint16_t ChunkStore::GetBlockAt(BlockPos pos) const {
    auto chunk = GetChunk({pos.x >> 5, pos.y >> 5, pos.z >> 5});
    return chunk->GetBlock(pos.x & 31, pos.y & 31, pos.z & 31);
}

uint8_t ChunkStore::GetMeta(int32_t x, int32_t y, int32_t z) const {
    auto chunk = GetChunk({x >> 5, y >> 5, z >> 5});
    return chunk->meta[(y & 31) << 10 | (z & 31) << 5 | (x & 31)];
}

uint32_t ChunkStore::GetMultiblock(int32_t x, int32_t y, int32_t z) const {
    auto chunk = GetChunk({x >> 5, y >> 5, z >> 5});
    return chunk->multiblock[(y & 31) << 10 | (z & 31) << 5 | (x & 31)];
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
    uint32_t idx = (static_cast<uint32_t>(ly) << 10) |
                   (static_cast<uint32_t>(lz) <<  5) |
                   (static_cast<uint32_t>(lx));
    int64_t key = makeKey(cx, cy, cz);

    Chunk* chunk = const_cast<Chunk*>(getCached(cx, cy, cz));
    if (!chunk) {
        Chunk local{};
        lmdb_.readRaw(key, local);
        local.blocks[idx] = id;
        local.meta[idx]   = meta;
        chunk = new Chunk(std::move(local));
        cache_.put(key, chunk);
        {
            std::lock_guard lock(encoder_.lmdb_palette_mutex_);
            encoder_.pending_lmdb_.erase(key);
        }
        markDirty(cx, cy, cz);
        return;
    }

    chunk->blocks[idx] = id;
    chunk->meta[idx]   = meta;
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

bool ChunkStore::SaveChunk(const Chunk& chunk, ChunkCoord coord) {
    std::vector<uint8_t> encoded;
    encodeChunk(chunk, encoded);
    return lmdb_.writeRaw(makeKey(coord.x, coord.y, coord.z),
                          encoded.data(), encoded.size());
}

const Chunk* ChunkStore::getCached(int32_t cx, int32_t cy, int32_t cz) const {
    return cache_.get(makeKey(cx, cy, cz));
}

void ChunkStore::putCached(int64_t key, Chunk* chunk) const {
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

void ChunkStore::AsyncSaveChunk(std::shared_ptr<const Chunk> chunk,
                                ChunkCoord coord,
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

void ChunkStore::enqueueEncode(ChunkCoord coord, Chunk* chunk) {
    encoder_.enqueueEncode(coord, chunk);
}

void ChunkStore::AsyncGetChunk(ChunkCoord coord, ChunkCallback callback) {
    int64_t key = makeKey(coord.x, coord.y, coord.z);

    if (auto* cached = getCached(coord.x, coord.y, coord.z)) {
        std::shared_ptr<std::vector<uint8_t>> palette;
        {
            std::lock_guard lock(encoder_.lmdb_palette_mutex_);
            auto it = encoder_.pending_lmdb_.find(key);
            if (it != encoder_.pending_lmdb_.end()) {
                palette = std::move(it->second);
                encoder_.pending_lmdb_.erase(it);
            }
        }
        if (palette) {
            callback(std::move(palette));
        } else {
            encoder_.encodeAndDeliver(cached, key, callback);
        }
        return;
    }

    asio::post(io_pool_, [this, coord, key, callback = std::move(callback)]() mutable {
        std::shared_ptr<std::vector<uint8_t>> palette;
        Chunk chunk;

        if (lmdb_.readRaw(key, chunk, &palette)) {
            auto* chunk_ptr = new Chunk(std::move(chunk));
            cache_.put(key, chunk_ptr);
            if (palette) {
                callback(std::move(palette));
            } else {
                encoder_.encodeAndDeliver(chunk_ptr, key, callback);
            }
            return;
        }
        {
            std::lock_guard lock(encoder_.encode_mutex_);
            encoder_.pending_gen_cbs_[key].push_back(std::move(callback));
        }
        gen_queue_->requestChunk(coord);
    });
}
