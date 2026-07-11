#include "CASHandler.h"
#include "../Chunk/Chunk.h"
#include "SectionCodec.h"
#include <spdlog/spdlog.h>

CASHandler::CASHandler(ChunkCache& cache, LmdbStore& lmdb,
                       NotifyFn notify)
    : cache_(cache), lmdb_(lmdb), notify_(std::move(notify)) {}

CASHandler::Result CASHandler::casBlock(int32_t x, int32_t y, int32_t z,
                                         uint16_t expected_id,
                                         uint16_t new_id, uint8_t new_meta) {
    int32_t cx = x >> 5, cy = y >> 5, cz = z >> 5;
    int32_t lx = x & 31, ly = y & 31, lz = z & 31;
    uint32_t idx = (static_cast<uint32_t>(ly) << 10) |
                   (static_cast<uint32_t>(lz) <<  5) |
                   (static_cast<uint32_t>(lx));
    int64_t key = LmdbStore::makeKey(cx, cy, cz);

    std::lock_guard lock(mutex_);

    Chunk* chunk = const_cast<Chunk*>(cache_.get(key));
    if (chunk == nullptr) {
        Chunk local{};
        if (!lmdb_.readRaw(key, local))
            return {Result::Conflict, 0, 0};
        uint16_t cur = local.blocks[idx];
        uint8_t  cur_m = local.meta[idx];
        if (cur != expected_id)
            return {Result::Conflict, cur, cur_m};
        local.blocks[idx] = new_id;
        local.meta[idx]   = new_meta;
        chunk = new Chunk(std::move(local));
        cache_.put(key, chunk);
        pending_[key].push_back({idx, new_id, new_meta});
        notify_(key);
        return {Result::Ok, new_id, new_meta};
    }

    uint16_t cur = chunk->blocks[idx];
    uint8_t  cur_m = chunk->meta[idx];
    if (cur != expected_id)
        return {Result::Conflict, cur, cur_m};

    chunk->blocks[idx] = new_id;
    chunk->meta[idx]   = new_meta;
    pending_[key].push_back({idx, new_id, new_meta});
    notify_(key);
    return {Result::Ok, new_id, new_meta};
}

size_t CASHandler::flush() {
    std::lock_guard lock(mutex_);
    if (pending_.empty()) return 0;

    size_t n_chunks = pending_.size();

    // Read each chunk's current data from LMDB, apply CAS changes, re-encode, write.
    for (auto& [key, changes] : pending_) {
        Chunk chunk;
        if (!lmdb_.readRaw(key, chunk)) {
            spdlog::warn("CASHandler::flush: chunk key {} not found in LMDB, skipping", key);
            continue;
        }
        for (auto& c : changes) {
            chunk.blocks[c.idx] = c.block_id;
            chunk.meta[c.idx]   = c.meta;
        }
        std::vector<uint8_t> encoded;
        encodeChunk(chunk, encoded);
        if (!lmdb_.writeRaw(key, encoded.data(), encoded.size())) {
            spdlog::error("CASHandler::flush: writeRaw failed for key {}", key);
        }
    }
    pending_.clear();
    return n_chunks;
}

void CASHandler::clear() {
    std::lock_guard lock(mutex_);
    pending_.clear();
}
