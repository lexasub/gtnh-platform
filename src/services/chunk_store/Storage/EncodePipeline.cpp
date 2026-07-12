#include "EncodePipeline.h"
#include "cache/ChunkCache.h"
#include "disk/LmdbStore.h"
#include "SectionCodec.h"
#include <pthread.h>
#include <spdlog/spdlog.h>

EncodePipeline::~EncodePipeline() {
    stop();
}

void EncodePipeline::start(ChunkCache& cache, LmdbStore& lmdb) {
    cache_ = &cache;
    lmdb_ = &lmdb;

    encode_threads_.push_back(std::thread([this] { encodeLoop(); }));
    encode_threads_.push_back(std::thread([this] { encodeLoop(); }));
}

void EncodePipeline::stop() {
    encode_running_ = false;
    encode_cv_.notify_all();
    for (auto& t : encode_threads_) {
        if (t.joinable()) t.join();
    }
    encode_threads_.clear();
}

void EncodePipeline::enqueueEncode(ChunkCoord coord, Chunk* chunk) {
    std::lock_guard lock(encode_mutex_);
    encode_queue_.push_back(EncodeTask{coord, chunk});
    encode_cv_.notify_one();
}

void EncodePipeline::encodeAndDeliver(const Chunk* chunk, int64_t key,
                                       ChunkCallback& callback) {
    std::vector<uint8_t> encoded;
    encodeChunk(*chunk, encoded);
    auto palette = std::make_shared<std::vector<uint8_t>>(std::move(encoded));
    {
        std::lock_guard lock(lmdb_palette_mutex_);
        pending_lmdb_.emplace(key, palette);
    }
    callback(std::move(palette));
}

void EncodePipeline::encodeLoop() {
    static std::atomic<int> thread_counter{0};
    int thread_id = thread_counter++;
    char name[16];
    snprintf(name, sizeof(name), "chunk-encode-%d", thread_id);
    pthread_setname_np(pthread_self(), name);

    std::vector<std::pair<int64_t, std::shared_ptr<std::vector<uint8_t>>>> local_palettes;
    local_palettes.reserve(64);

    while (encode_running_) {
        std::deque<EncodeTask> tasks;
        {
            std::unique_lock lock(encode_mutex_);
            encode_cv_.wait(lock, [this] {
                return !encode_running_.load() || !encode_queue_.empty();
            });
            if (!encode_running_ && encode_queue_.empty()) break;
            tasks.swap(encode_queue_);
        }
        while (!tasks.empty()) {
            auto task = std::move(tasks.front());
            tasks.pop_front();
            std::vector<uint8_t> encoded;
            encoded.reserve(Chunk::VOLUME / 8); // TODO подобрать значение
            encodeChunk(*task.chunk, encoded);
            encoded.shrink_to_fit();
            auto palette = std::make_shared<std::vector<uint8_t>>(std::move(encoded));
            int64_t key = LmdbStore::makeKey(task.coord.x, task.coord.y, task.coord.z);

            cache_->put(key, task.chunk);

            /*{ //in my opinion it's not needed
                std::lock_guard lock(lmdb_palette_mutex_);
                pending_lmdb_.emplace(key, std::move(palette));
            }*/

            local_palettes.emplace_back(key, palette);

            std::vector<ChunkCallback> callbacks;
            {
                std::lock_guard lock(encode_mutex_);
                if (auto it = pending_gen_cbs_.find(key); it != pending_gen_cbs_.end()) {
                    callbacks = std::move(it->second);
                    pending_gen_cbs_.erase(it);
                }
            }
            callbacks.front()(palette);
            if (callbacks.size() > 1) [[unlikely]] {
                for (size_t i = 1; i < callbacks.size(); ++i) {
                    callbacks[i](palette);
                }
            }
        }
        if (!local_palettes.empty()) [[unlikely]] {
            lmdb_->writeBatch(local_palettes);
        }
        if (size_t size = local_palettes.size(); size > 128) {
            local_palettes.resize(std::max(size / 2, static_cast<size_t>(64))); // подрезаем в 2 раза, надеемся что следующий батч будет меньше
            local_palettes.shrink_to_fit();
        }
    }
    if (!local_palettes.empty()) {
        lmdb_->writeBatch(local_palettes);
    }
}
