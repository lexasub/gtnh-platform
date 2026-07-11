#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

class ChunkCache;
class LmdbStore;

// Tracks dirty chunks and periodically flushes them to LMDB.
// Shared with EncodePipeline via pending_lmdb_ (must be set after start()).
class FlushPipeline {
public:
    explicit FlushPipeline() = default;
    ~FlushPipeline();

    void start(ChunkCache& cache, LmdbStore& lmdb,
               std::unordered_map<int64_t,
                   std::shared_ptr<std::vector<uint8_t>>>* pending_lmdb,
               std::mutex* pending_lmdb_mutex);
    void stop();

    void markDirty(int64_t key);
    bool flushDirtyChunks();

    static constexpr auto FLUSH_INTERVAL = std::chrono::milliseconds(1500);
    static constexpr size_t DEFAULT_MAX_MAP_SIZE =
        256ULL * 1024 * 1024 * 1024;

private:
    void flushThreadFunc();

    ChunkCache* cache_ = nullptr;
    LmdbStore* lmdb_ = nullptr;

    // Pointer to EncodePipeline's pending_lmdb_ for palette lookup.
    std::unordered_map<int64_t, std::shared_ptr<std::vector<uint8_t>>>* pending_lmdb_ = nullptr;
    std::mutex* pending_lmdb_mutex_ = nullptr;

    std::unordered_set<int64_t> dirty_chunks_;
    mutable std::mutex dirty_mutex_;
    std::thread flush_thread_;
    std::atomic<bool> flush_running_{true};
    std::mutex flush_wake_mutex_;
    std::condition_variable flush_wake_cv_;
};
