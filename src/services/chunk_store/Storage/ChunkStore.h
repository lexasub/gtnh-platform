#pragma once

#include "ClockCache.h"
#include "IBlockStore.h"
#include "WorkQueue.h"
#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <deque>
#include <condition_variable>
#include <functional>
#include <lmdb.h>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_set>

class ChunkStore : public IBlockStore {
public:
  // Callback type: (flat Chunk*, palette-encoded sections) — palette is ready
  // for network send, no re-encode needed. Palette is cached for flush.
    using ChunkCallback =
      std::move_only_function<void(std::shared_ptr<std::vector<uint8_t>>)>;

  explicit ChunkStore(const std::string &db_path, size_t cache_size = 1024,
                      size_t max_map_size = DEFAULT_MAX_MAP_SIZE);
  ~ChunkStore() override;

  bool HasChunk(ChunkCoord c) const override;
  const Chunk *GetChunk(ChunkCoord c) const override; // опасно, но оставляем
  uint16_t GetBlockAt(BlockPos pos) const override;
  void SetBlock(ChunkCoord coord, BlockPos pos, uint16_t blockId, uint8_t meta,
                uint32_t mbId) override; // синхронный
  bool SaveChunk(const Chunk &chunk, ChunkCoord coord) override;

  const Chunk *getCached(int32_t cx, int32_t cy, int32_t cz) const;

  void putCached(int32_t cx, int32_t cy, int32_t cz, Chunk *chunk) const;

  uint8_t getMeta(int32_t x, int32_t y, int32_t z);

  uint32_t getMultiblock(int32_t x, int32_t y, int32_t z);

  void setBlock(int32_t x, int32_t y, int32_t z, uint16_t id, uint8_t meta);

  // Асинхронные методы (callback-based, используют I/O pool вместо std::async)
  /// @see ServerWorld.h for Chunk* lifetime contract
  /// The pointer passed to the callback is valid ONLY during the synchronous
  /// invocation of the callback. Do NOT store or use after returning.
  /// palette_out is valid for the same duration — move/copy to retain.
  void AsyncGetChunk(ChunkCoord coord, ChunkCallback callback);
  void AsyncSetBlock(ChunkCoord coord, BlockPos pos, uint16_t blockId,
                     uint8_t meta, uint32_t mbId,
                     std::function<void(bool)> callback = nullptr);
  void AsyncSaveChunk(std::shared_ptr<const Chunk> chunk, ChunkCoord coord,
                      std::function<void(bool)> callback = nullptr);

  // Чтение метаданных (быстро, из кэша или LMDB read-only)
  uint8_t GetMeta(int32_t x, int32_t y, int32_t z) const;
  uint32_t GetMultiblock(int32_t x, int32_t y, int32_t z) const;

  // Подключение очереди генерации (из world_generator)
  void SetGenerationQueue(class GenerationQueue *gen) { gen_queue_ = gen; }

  // Called by ServerWorld when gen thread finishes generating a chunk.
  // Pushes to encode queue; encode thread encodes + delivers + stores palette.
  void enqueueEncode(ChunkCoord coord, Chunk* chunk);

  // Dirty-chunk tracking for batch LMDB flush
  void markDirty(int32_t cx, int32_t cy, int32_t cz);
  void flushDirtyChunks();

  static constexpr size_t DEFAULT_MAX_MAP_SIZE =
      256ULL * 1024 * 1024 * 1024; // 256 GB
  static constexpr auto FLUSH_INTERVAL = std::chrono::milliseconds(1500);

private:
  void open();
  void close();
  static int64_t makeKey(int32_t cx, int32_t cy, int32_t cz);

  bool growMapSize();

  // Lock-free кэш (сырые указатели, владеет Chunk*, delete на вытеснение)
  mutable ClockCache<uintptr_t, 1024> cache_;

  // LMDB окружение
  MDB_env *env_ = nullptr;
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
  class GenerationQueue *gen_queue_ = nullptr;

  // Dirty-chunk tracking for batch LMDB flush
  mutable std::unordered_set<int64_t> dirty_chunks_;
  mutable std::mutex dirty_mutex_;
  std::thread flush_thread_;
  std::atomic<bool> flush_running_{true};
  std::mutex flush_wake_mutex_;
  std::condition_variable flush_wake_cv_;

  void flushThreadFunc();

  // Encode queue: gen threads push (coord, flat chunk), encode thread pops,
  // encodes once, delivers palette to callbacks + LMDB flush
  struct EncodeTask {
    ChunkCoord coord;
    Chunk* chunk;
  };
  std::deque<EncodeTask> encode_queue_;
  mutable std::mutex encode_mutex_;
  std::condition_variable encode_cv_;
  std::vector<std::thread> encode_threads_;
  std::atomic<bool> encode_running_{true};
  void encodeLoop();
  void encodeAndDeliver(const Chunk* chunk, int64_t key,
                        ChunkCallback& callback);

  // Pending callbacks for worldgen (coord key → callbacks waiting on encode)
  mutable std::unordered_map<int64_t, std::vector<ChunkCallback>>
      pending_gen_cbs_;

  // Pre-encoded palette for flush thread (worldgen path: encode already done)
  mutable std::unordered_map<int64_t, std::shared_ptr<std::vector<uint8_t>>>
      pending_lmdb_;
  mutable std::mutex lmdb_palette_mutex_;

  // Чтение/запись транзакций (низкоуровневые)
  bool readTransaction(int64_t key, Chunk &out,
      std::shared_ptr<std::vector<uint8_t>>* palette_out = nullptr) const;

  // Write raw palette bytes directly (avoids re-encode on flush)
  bool writeTransaction(int64_t key, const uint8_t* data, size_t size);
};