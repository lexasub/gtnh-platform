#pragma once

#include "cache/ChunkCache.h"
#include "disk/LmdbStore.h"
#include "EncodePipeline.h"
#include "disk/FlushPipeline.h"
#include "CASHandler.h"
#include <common/coords/Coords.h>
#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

class GenerationQueue;

// Facade over 4 storage components: cache, LMDB, encode pipeline, flush pipeline.
// Single translation unit (ChunkStore.cpp) for full inlining.
// Does NOT implement IBlockStore — LmdbStore does.
class ChunkStore {
public:
  using ChunkCallback = EncodePipeline::ChunkCallback;

  explicit ChunkStore(const std::string& db_path, size_t cache_size = 1024,
                      size_t max_map_size = DEFAULT_MAX_MAP_SIZE);
  ~ChunkStore();

  // --- Synchronous (fast path, <1us on cache hit) ---
  bool HasChunk(ChunkCoord c) const;
  const Chunk* GetChunk(ChunkCoord c) const;
  uint16_t GetBlockAt(BlockPos pos) const;
  uint8_t GetMeta(int32_t x, int32_t y, int32_t z) const;
  uint32_t GetMultiblock(int32_t x, int32_t y, int32_t z) const;

  // setBlock — cache + modify + mark dirty. Does NOT persist immediately.
  void setBlock(int32_t x, int32_t y, int32_t z, uint16_t id, uint8_t meta);

  // casBlock — delegate to CASHandler. Returns status + actual values.
  using CASResult = CASHandler::Result;
  CASResult casBlock(int32_t x, int32_t y, int32_t z,
                       uint16_t expected_id, uint16_t new_id, uint8_t new_meta);

  // SetBlock — sync wrapper, posts to I/O pool internally.
  void SetBlock(ChunkCoord coord, BlockPos pos, uint16_t blockId,
                  uint8_t meta, uint32_t mbId);
  bool SaveChunk(const Chunk& chunk, ChunkCoord coord);

  // --- Asynchronous (callback-based) ---
  void AsyncGetChunk(ChunkCoord coord, ChunkCallback callback);
  void AsyncSetBlock(ChunkCoord coord, BlockPos pos, uint16_t blockId,
                     uint8_t meta, uint32_t mbId,
                     std::function<void(bool)> callback = nullptr);
  void AsyncSaveChunk(std::shared_ptr<const Chunk> chunk, ChunkCoord coord,
                      std::function<void(bool)> callback = nullptr);

  // --- Cache access (for encode pipeline) ---
  const Chunk* getCached(int32_t cx, int32_t cy, int32_t cz) const;
  const Chunk* getCachedPinned(int32_t cx, int32_t cy, int32_t cz);
  void releaseCachedPinned(const Chunk* chunk);
  void putCached(int64_t key, Chunk* chunk) const;

  // Called by worldgen thread when chunk generation is done.
  void enqueueEncode(ChunkCoord coord, Chunk* chunk);

  // --- Dirty-tracking (for CASHandler) ---
  void markDirty(int32_t cx, int32_t cy, int32_t cz);
  bool flushDirtyChunks();

  static constexpr size_t DEFAULT_MAX_MAP_SIZE =
      256ULL * 1024 * 1024 * 1024;
  static constexpr auto FLUSH_INTERVAL = std::chrono::milliseconds(1500);

private:
  void close();
  static int64_t makeKey(int32_t cx, int32_t cy, int32_t cz)
      { return LmdbStore::makeKey(cx, cy, cz); }

  // The 5 components
  mutable ChunkCache cache_;
  LmdbStore lmdb_;
  EncodePipeline encoder_;
  FlushPipeline flusher_;
  CASHandler cas_;
  WorldGenerator* generator_;

  // I/O pool for async operations (not owned by any single component)
  mutable asio::io_context io_pool_;
  using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;
  mutable WorkGuard work_guard_{asio::make_work_guard(io_pool_)};
  mutable std::vector<std::thread> io_threads_;

  class GenerationQueue* gen_queue_ = nullptr;
};
