#include "FlushPipeline.h"
#include "../cache/ChunkCache.h"
#include "LmdbStore.h"
#include "../SectionCodec.h"
#include <pthread.h>
#include <spdlog/spdlog.h>

FlushPipeline::~FlushPipeline() {
    stop();
}

void FlushPipeline::start(ChunkCache& cache, LmdbStore& lmdb,
                           std::unordered_map<int64_t,
                               std::shared_ptr<std::vector<uint8_t>>>* pending_lmdb,
                           std::mutex* pending_lmdb_mutex) {
    cache_ = &cache;
    lmdb_ = &lmdb;
    pending_lmdb_ = pending_lmdb;
    pending_lmdb_mutex_ = pending_lmdb_mutex;
    flush_thread_ = std::thread([this] { flushThreadFunc(); });
}

void FlushPipeline::stop() {
    flush_running_ = false;
    flush_wake_cv_.notify_one();
    if (flush_thread_.joinable()) flush_thread_.join();
    flushDirtyChunks();
}

void FlushPipeline::markDirty(int64_t key) {
    std::lock_guard lock(dirty_mutex_);
    dirty_chunks_.insert(key);
}

bool FlushPipeline::flushDirtyChunks() {
    std::unordered_set<int64_t> local;
    {
        std::lock_guard lock(dirty_mutex_);
        local.swap(dirty_chunks_);
    }
    if (local.empty()) [[unlikely]] return false;

    size_t saved = 0;
    for (const auto& key : local) {
        std::shared_ptr<std::vector<uint8_t>> palette;
        {
            std::lock_guard lock(*pending_lmdb_mutex_);
            auto it = pending_lmdb_->find(key);
            if (it != pending_lmdb_->end()) {
                palette = it->second;
                pending_lmdb_->erase(it);
            }
        }
        if (palette) {
            if (lmdb_->writeRaw(key, palette->data(), palette->size())) {
                ++saved;
            }
        } else {
            const Chunk* chunk = cache_->get(key);
            if (!chunk) continue;
            std::vector<uint8_t> encoded;
            encodeChunk(*chunk, encoded);
            if (lmdb_->writeRaw(key, encoded.data(), encoded.size())) [[likely]] {
                ++saved;
            }
        }
    }
    if (saved > 0) [[likely]]
        spdlog::debug("FlushPipeline: flushed {} dirty chunks to LMDB", saved);
    return saved > 0;
}

void FlushPipeline::flushThreadFunc() {
    pthread_setname_np(pthread_self(), "chunk-flush");
    while (flush_running_) {
        {
            std::unique_lock lock(flush_wake_mutex_);
            flush_wake_cv_.wait_for(lock, FLUSH_INTERVAL,
                [this]{ return !flush_running_.load(); });
        }
        if (!flush_running_) break;
        flushDirtyChunks();
    }
}
