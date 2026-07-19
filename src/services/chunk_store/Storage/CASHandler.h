#pragma once

#include "cache/ChunkCache.h"
#include "cache/MutableChunk.h"
#include "disk/LmdbStore.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

// Compare-and-swap block updates with deferred batch flush to LMDB.
//
// CASHandler owns a queue of pending CAS mutations per chunk key.
// casBlock() mutates the cache synchronously and queues the change.
// flush() writes all queued changes to LMDB in a single transaction.
//
// Thread-safe: casBlock() and flush() are serialized via mutex_.
//
// NotifyFn is called after each successful CAS mutation to:
//   1. Invalidate any stale palette in the encode pipeline
//   2. Mark the chunk dirty for the flush pipeline
class CASHandler {
public:
    using NotifyFn = std::move_only_function<void(int64_t key)>;

    explicit CASHandler(ChunkCache& cache, LmdbStore& lmdb,
                        NotifyFn notify = nullptr);

    // CAS result identical to ChunkStore::CASResult
    struct Result {
        enum Status { Conflict, Ok };
        Status status;
        uint16_t actual_block;
        uint8_t  actual_meta;
    };

    // Check-and-swap on cache. If chunk not loaded — error (no auto-load).
    // On success: mutates cache, queues chunk key for batch flush.
    // On conflict: returns current values, no mutation.
    Result casBlock(int32_t x, int32_t y, int32_t z,
                    uint16_t expected_id, uint16_t new_id, uint8_t new_meta);

    // Flush all queued CAS mutations to LMDB in a single transaction.
    // Returns number of chunks flushed (0 if queue empty).
    size_t flush();

    // Clear pending queue without flushing (called on shutdown).
    void clear();

private:
    struct PendingChange {
        uint32_t idx;       // local index in chunk
        uint16_t block_id;
        uint8_t  meta;
    };

    ChunkCache& cache_;
    LmdbStore&  lmdb_;
    NotifyFn    notify_;

    std::mutex mutex_;
    // chunk key -> vector of pending CAS mutations for that chunk
    std::unordered_map<int64_t, std::vector<PendingChange>> pending_;
};
