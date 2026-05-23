#pragma once

#include "IBlockStore.h"
#include "ClockCache.h"
#include "WorkQueue.h"
#include <lmdb.h>
#include <atomic>
#include <thread>
#include <queue>
#include <unordered_set>
#include <functional>
#include <asio.hpp>
#include <mutex>
#include <condition_variable>
#include <chrono>

class ChunkStore : public IBlockStore {
public:
    explicit ChunkStore(const std::string& db_path, size_t cache_size = 1024, size_t max_map_size = DEFAULT_MAX_MAP_SIZE);
    ~ChunkStore() override;

    bool HasChunk(ChunkCoord c) const override;
    const Chunk* GetChunk(ChunkCoord c) const override;   // опасно, но оставляем
    uint16_t GetBlockAt(BlockPos pos) const override;
    void SetBlock(ChunkCoord coord, BlockPos pos, uint16_t blockId, uint8_t meta, uint32_t mbId) override; // синхронный
    bool SaveChunk(const Chunk& chunk, ChunkCoord coord) override;

    const Chunk* getCached(int32_t cx, int32_t cy, int32_t cz) const;

    void putCached(int32_t cx, int32_t cy, int32_t cz, Chunk* chunk) const;

    uint8_t getMeta(int32_t x, int32_t y, int32_t z);

    uint32_t getMultiblock(int32_t x, int32_t y, int32_t z);

    void setBlock(int32_t x, int32_t y, int32_t z, uint16_t id, uint8_t meta);

    // Асинхронные методы (callback-based, используют I/O pool вместо std::async)
    /// @see ServerWorld.h for Chunk* lifetime contract
    /// The pointer passed to the callback is valid ONLY during the synchronous
    /// invocation of the callback. Do NOT store or use after returning.
    void AsyncGetChunk(ChunkCoord coord,
                       std::move_only_function<void(const Chunk*)> callback);
    void AsyncSetBlock(ChunkCoord coord, BlockPos pos, uint16_t blockId, uint8_t meta, uint32_t mbId,
                       std::function<void(bool)> callback = nullptr);
    void AsyncSaveChunk(std::shared_ptr<const Chunk> chunk, ChunkCoord coord,
                        std::function<void(bool)> callback = nullptr);

    // Чтение метаданных (быстро, из кэша или LMDB read-only)
    uint8_t GetMeta(int32_t x, int32_t y, int32_t z) const;
    uint32_t GetMultiblock(int32_t x, int32_t y, int32_t z) const;

    // Подключение очереди генерации (из world_generator)
    void SetGenerationQueue(class GenerationQueue* gen) { gen_queue_ = gen; }

    // Dirty-chunk tracking for batch LMDB flush
    void markDirty(int32_t cx, int32_t cy, int32_t cz);
    void flushDirtyChunks();

    static constexpr size_t DEFAULT_MAX_MAP_SIZE = 256ULL * 1024 * 1024 * 1024; // 256 GB
    static constexpr auto FLUSH_INTERVAL = std::chrono::milliseconds(1500);

private:
    void open();
    void close();
    static int64_t makeKey(int32_t cx, int32_t cy, int32_t cz);

    bool growMapSize();

    // Lock-free кэш (сырые указатели, владеет Chunk*, delete на вытеснение)
    mutable ClockCache<uintptr_t, 1024> cache_;

    // LMDB окружение
    MDB_env* env_ = nullptr;
    MDB_dbi dbi_ = 0;
    std::string db_path_;
    size_t maxMapSize_ = DEFAULT_MAX_MAP_SIZE;

    // Serialise concurrent resize attempts
    mutable std::mutex resizeMutex_;

    // I/O thread pool (asio::io_context + 2 threads, replaces std::async)
    mutable asio::io_context io_pool_;
    using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;
    mutable WorkGuard work_guard_{asio::make_work_guard(io_pool_)};
    mutable std::vector<std::thread> io_threads_;

    // Указатель на очередь генерации (не владеет)
    class GenerationQueue* gen_queue_ = nullptr;

    // Dirty-chunk tracking for batch LMDB flush
    mutable std::unordered_set<int64_t> dirty_chunks_;
    mutable std::mutex dirty_mutex_;
    std::thread flush_thread_;
    std::atomic<bool> flush_running_{true};
    std::mutex flush_wake_mutex_;
    std::condition_variable flush_wake_cv_;

    void flushThreadFunc();

    // Чтение/запись транзакций (низкоуровневые)
    bool readTransaction(int64_t key, Chunk& out) const;
    bool writeTransaction(int64_t key, const Chunk& chunk);
};