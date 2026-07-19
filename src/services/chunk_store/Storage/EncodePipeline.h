#pragma once

#include "../Chunk/Chunk.h"
#include "../../world_generator/GenerationQueue.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

class ChunkCache;
class LmdbStore;

struct EncodeTask {
    ChunkCoord coord;
    Chunk* chunk;
};

// Owns encode threads + palette cache + pending generation callbacks.
// After encoding a chunk, stores palette and calls on_encoded callback.
//
// Template-free design: references to cache/lmdb stored as pointers.
// All methods called from encode threads or I/O pool threads.
class EncodePipeline {
public:
    using ChunkCallback =
        std::move_only_function<void(std::shared_ptr<std::vector<uint8_t>>)>;

    EncodePipeline() = default;
    ~EncodePipeline();

    void start(ChunkCache& cache, LmdbStore& lmdb);
    void stop();

    // Called by worldgen thread when chunk generation is done.
    void enqueueEncode(ChunkCoord coord, Chunk* chunk);

    // For gen queue wiring
    void SetGenerationQueue(GenerationQueue* gen) { gen_queue_ = gen; }

    void markAsPendingLmdb(std::shared_ptr<std::vector<uint8_t>> palette, int64_t key);

    std::shared_ptr<std::vector<uint8_t>> takePendingPalette(int64_t key);

    // Pending callbacks for worldgen (coord key -> callbacks waiting on encode).
    // Shared with AsyncGetChunk — accessed under encode_mutex_.
    std::unordered_map<int64_t, std::vector<ChunkCallback>> pending_gen_cbs_;

    // Pre-encoded palette for flush thread.
    // Shared with FlushPipeline under lmdb_palette_mutex_.
    std::unordered_map<int64_t, std::shared_ptr<std::vector<uint8_t>>>
        pending_lmdb_;
    std::mutex lmdb_palette_mutex_;

    mutable std::mutex encode_mutex_;
private:
    void encodeLoop();

    ChunkCache* cache_ = nullptr;
    LmdbStore* lmdb_ = nullptr;
    GenerationQueue* gen_queue_ = nullptr;

    std::deque<EncodeTask> encode_queue_;
    std::condition_variable encode_cv_;
    std::vector<std::thread> encode_threads_;
    std::atomic<bool> encode_running_{true};
};
