#pragma once

#include "../chunk_store/Chunk/Chunk.h"
#include <common/coords/Coords.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class WorldGenerator;

class GenerationQueue {
public:
    GenerationQueue(WorldGenerator* generator, size_t num_threads = 8); // TODO - dynamic threads
    ~GenerationQueue();

    void requestChunk(ChunkCoord coord, std::move_only_function<void(std::shared_ptr<Chunk>)> callback);
    void stop();

private:
    struct PendingEntry {
        std::vector<std::move_only_function<void(std::shared_ptr<Chunk>)>> callbacks;
    };
    struct ChunkCoordHash {
        size_t operator()(const ChunkCoord& c) const noexcept {
            uint64_t h = 0xcbf29ce484222325ull;
            h = (h ^ static_cast<uint64_t>(c.x)) * 0x100000001b3ull;
            h = (h ^ static_cast<uint64_t>(c.y)) * 0x100000001b3ull;
            h = (h ^ static_cast<uint64_t>(c.z)) * 0x100000001b3ull;
            return h;
        }
    };

    void workerLoop();

    WorldGenerator* generator_;      // not owned
    std::vector<std::thread> workers_;
    std::queue<ChunkCoord> tasks_;
    std::unordered_map<ChunkCoord, PendingEntry, ChunkCoordHash> pending_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{false};
};
