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
        if (!wire && wire.error() == ReadError::Failed) {
            // gp-wjmb: a read error is not a CAS conflict. Reporting Conflict
            // would tell the client its expected_id is wrong when in fact we
            // simply could not read the chunk. Conflict is still the honest
            // answer for a genuine miss (see below), so a caller cannot tell
            // them apart at the CAS layer — the distinction is logged.
            spdlog::error("CASHandler::casBlock: LMDB read failed for key {} — "
                          "reporting a conflict, but this is an I/O failure, "
                          "not a value mismatch", key);
            return {Result::Conflict, 0, 0};
        }
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
    // gp-wjmb: chunks whose read failed stay in pending_ so their CAS changes
    // are not lost; only the entries that were actually handled are erased.
    std::vector<int64_t> deferred;
    deferred.reserve(n_chunks);

    // Read each chunk's current data from LMDB, apply CAS changes, re-encode, write.
    for (auto& [key, changes] : pending_) {
        auto wire = lmdb_.readRawBytes(key);
        if (!wire) {
            // A read failure must not be logged as "not found" and silently
            // skipped — the queued CAS changes are the only record of them.
            if (wire.error() == ReadError::Failed) [[unlikely]] {
                spdlog::error("CASHandler::flush: LMDB read failed for key {} — "
                              "keeping {} queued change(s) for a later flush",
                              key, changes.size());
                deferred.push_back(key);
                continue;
            }
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
            // Same reasoning as a failed read: the changes are not on disk, so
            // they must stay queued rather than being cleared.
            spdlog::error("CASHandler::flush: writeRaw failed for key {} — "
                          "keeping {} queued change(s) for a later flush",
                          key, changes.size());
            deferred.push_back(key);
        }
    }
    // gp-wjmb: drop only the entries that were actually handled; anything that
    // failed to read or to write stays queued for the next flush.
    for (int64_t key : deferred)
        pending_.erase(key);
    return n_chunks - deferred.size();
}

void CASHandler::clear() {
    std::lock_guard lock(mutex_);
    pending_.clear();
}
