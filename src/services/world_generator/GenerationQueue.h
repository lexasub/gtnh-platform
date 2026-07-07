#pragma once

#include "../chunk_store/Chunk/Chunk.h"
#include <atomic>
#include <common/coords/Coords.h>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <unordered_set>

class WorldGenerator;

class GenerationQueue {
public:
  /// Output callback: gen thread calls this when a chunk is generated.
  /// ChunkStore sets this to push into the encode queue.
  using GenOutput =
      std::move_only_function<void(ChunkCoord, Chunk*)>;

  GenerationQueue(WorldGenerator *generator, GenOutput output,
                  size_t num_threads = 8); // TODO - dynamic threads
  ~GenerationQueue();

  /// Push coord for generation. No callback — generated chunk goes to
  /// GenOutput, then to encode queue, then callbacks ChunkCallback.
  void requestChunk(ChunkCoord coord);
  void stop();

private:
  struct ChunkCoordHash {
    size_t operator()(const ChunkCoord &c) const noexcept {
      uint64_t h = 0xcbf29ce484222325ull;
      h = (h ^ static_cast<uint64_t>(c.x)) * 0x100000001b3ull;
      h = (h ^ static_cast<uint64_t>(c.y)) * 0x100000001b3ull;
      h = (h ^ static_cast<uint64_t>(c.z)) * 0x100000001b3ull;
      return h;
    }
  };

  void workerLoop();

  WorldGenerator *generator_; // not owned
  std::vector<std::thread> workers_;
  std::queue<ChunkCoord> tasks_;
  GenOutput output_;
  std::unordered_set<ChunkCoord, ChunkCoordHash> dedup_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> stop_{false};
};
