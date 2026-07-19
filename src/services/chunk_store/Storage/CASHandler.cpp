#include "CASHandler.h"
#include "cache/MutableChunk.h"
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

    MutableChunk* chunk = const_cast<MutableChunk*>(cache_.get(key));
    if (chunk == nullptr) {
        auto wire = lmdb_.readRawBytes(key);
        if (!wire)
            return {Result::Conflict, 0, 0};
        MutableChunk local;
        if (!local.fromWire(wire->data(), wire->size()))
            return {Result::Conflict, 0, 0};
        uint16_t cur = local.getBlock(lx, ly, lz);
        uint8_t  cur_m = local.getMeta(lx, ly, lz);
        if (cur != expected_id)
            return {Result::Conflict, cur, cur_m};
        local.setBlock(lx, ly, lz, new_id);
        local.setMeta(lx, ly, lz, new_meta);
        chunk = new MutableChunk(std::move(local));
        cache_.put(key, chunk);
        pending_[key].push_back({idx, new_id, new_meta});
        notify_(key);
        return {Result::Ok, new_id, new_meta};
    }

    uint16_t cur = chunk->getBlock(lx, ly, lz);
    uint8_t  cur_m = chunk->getMeta(lx, ly, lz);
    if (cur != expected_id)
        return {Result::Conflict, cur, cur_m};

    chunk->setBlock(lx, ly, lz, new_id);
    chunk->setMeta(lx, ly, lz, new_meta);
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
        auto wire = lmdb_.readRawBytes(key);
        if (!wire) {
            spdlog::warn("CASHandler::flush: chunk key {} not found in LMDB, skipping", key);
            continue;
        }
        MutableChunk mc;
        if (!mc.fromWire(wire->data(), wire->size())) {
            spdlog::warn("CASHandler::flush: failed to decode key {}", key);
            continue;
        }
        for (auto& c : changes) {
            int lx = c.idx & 0xF;
            int lz = (c.idx >> 4) & 0xF;
            int ly = (c.idx >> 8) & 0xF;
            mc.setBlock(lx, ly, lz, c.block_id);
            mc.setMeta(lx, ly, lz, c.meta);
        }
        std::vector<uint8_t> encoded;
        mc.encodeToWire(encoded);
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
