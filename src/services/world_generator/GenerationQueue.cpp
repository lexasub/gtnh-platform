#include "GenerationQueue.h"
#include "WorldGenerator.h"
#include <pthread.h>
#include <spdlog/spdlog.h>
#include <cstdio>
#include <utility>

GenerationQueue::GenerationQueue(WorldGenerator* generator, GenOutput output, size_t num_threads)
    : generator_(generator), output_(std::move(output)) {
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&GenerationQueue::workerLoop, this);
    }
    spdlog::info("GenerationQueue started with {} worker threads", num_threads);
}

GenerationQueue::~GenerationQueue() {
    stop();
}

void GenerationQueue::requestChunk(ChunkCoord coord) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (dedup_.count(coord))
            return;
        dedup_.insert(coord);
        tasks_.push(coord);
    }
    cv_.notify_one();
}

void GenerationQueue::stop() {
    if (stop_.exchange(true)) return;
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    spdlog::info("GenerationQueue stopped");
}

void GenerationQueue::workerLoop() {
    static std::atomic<int> thread_counter{0};
    int thread_id = thread_counter++;
    char name[16];
    snprintf(name, sizeof(name), "chunk-gen-%d", thread_id);
    pthread_setname_np(pthread_self(), name);

    while (!stop_.load()) {
        ChunkCoord chunkCoord;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !tasks_.empty() || stop_.load(); });
            if (stop_.load() && tasks_.empty()) break;
            chunkCoord = std::move(tasks_.front());
            tasks_.pop();
        }

        auto chunk = new Chunk;
        generator_->GenerateTerrain(*chunk, chunkCoord.x, chunkCoord.y, chunkCoord.z);
        //spdlog::debug("Generated chunk ({},{},{})", chunkCoord.x, chunkCoord.y, chunkCoord.z);

        std::vector<std::move_only_function<void(std::shared_ptr<Chunk>)>> cbs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dedup_.erase(chunkCoord);
        }

        if (output_)
            output_(chunkCoord, chunk);
    }
}
